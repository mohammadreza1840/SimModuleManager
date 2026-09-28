const http = require('node:http');
const crypto = require('node:crypto');
const fs = require('node:fs');
const config = require('../config');
const { Pcm16Assembler, ByteRingBuffer } = require('../audio/pcm');
const { WavAppender } = require('../audio/wav');
const { safeCallPath } = require('./storage');

const EventEmitter = require('node:events');

function wait(ms) { return new Promise(resolve => setTimeout(resolve, ms)); }

class DeviceReceiver extends EventEmitter {
    constructor(prisma, device, owner, options = {}) {
        super();
        this.prisma = prisma;
        this.device = device;
        this.owner = owner;
        this.stopped = false;
        this.request = null;
        this.writer = null;
        this.callId = null;
        this.backoff = 500;
        this.assembler = new Pcm16Assembler();
        const preRollMs = options.preRollMs || 2000;
        this.ring = new ByteRingBuffer(Math.floor(device.input_sample_rate * 2 * preRollMs / 1000));
        this.maxCallBytes = options.maxCallBytes || 536870912;
        this.preRollMs = preRollMs;
        this.nextSampleCounter = null;
        this.writerStartCounter = null;
        this.counterGap = false;
        this.limitReached = false;
        this.callPoll = setInterval(() => this.syncCall().catch(error => this.setError(error.message)), 200);
        this.leaseTimer = setInterval(() => this.renewLease().catch(() => this.stop()), Math.max(1000, config.receiverLeaseMs / 3));
    }

    async run() {
        while (!this.stopped) {
            try {
                await this.connectOnce();
                this.backoff = 500;
            } catch (error) {
                if (this.stopped) break;
                await this.noteDisconnect(error);
                const jitter = Math.floor(Math.random() * Math.max(100, this.backoff / 3));
                await wait(this.backoff + jitter);
                this.backoff = Math.min(30000, this.backoff * 2);
            }
        }
    }

    connectOnce() {
        return new Promise((resolve, reject) => {
            const request = http.get({
                hostname: this.device.ip_address,
                port: 8080,
                path: '/',
                timeout: config.streamTimeoutMs,
                headers: { Accept: 'application/octet-stream' },
            });
            this.request = request;
            request.on('response', response => {
                if (response.statusCode !== 200) {
                    response.resume();
                    const error = new Error(response.statusCode === 409 ? 'Device stream already has a listener (HTTP 409)' : `Audio stream HTTP ${response.statusCode}`);
                    error.code = response.statusCode === 409 ? 'STREAM_CONFLICT' : 'STREAM_HTTP';
                    reject(error);
                    return;
                }
                // A new HTTP response begins on a device-declared sample boundary.
                // Never combine a half sample left by a broken TCP connection with it.
                this.assembler.reset();
                this.backoff = 500;
                this.device.stream_session_id = response.headers['x-audio-session-id'] || null;
                const announcedCounter = response.headers['x-audio-start-sample'];
                if (announcedCounter !== undefined && /^\d+$/.test(String(announcedCounter))) {
                    const next = BigInt(announcedCounter);
                    if (this.nextSampleCounter !== null && next > this.nextSampleCounter && this.callId) this.counterGap = true;
                    this.nextSampleCounter = next;
                } else {
                    this.nextSampleCounter = null;
                }
                this.setStatus('connected', null, true).catch(() => {});
                response.on('data', chunk => {
                    const pcm = this.assembler.push(chunk);
                    if (!pcm.length) return;
                    this.ring.push(pcm);
                    if (this.writer && !this.limitReached) {
                        const remaining = this.maxCallBytes - this.writer.dataBytes;
                        if (remaining <= 0) {
                            this.markLimitExceeded().catch(() => {});
                        } else {
                            this.writer.append(pcm.subarray(0, Math.min(pcm.length, remaining & ~1)));
                        }
                    }
                    if (this.nextSampleCounter !== null) this.nextSampleCounter += BigInt(pcm.length / 2);
                    this.emit('audio', pcm);
                    this.throttledAudioHeartbeat();
                });
                response.on('end', () => reject(new Error('Audio stream ended')));
                response.on('error', reject);
            });
            request.on('timeout', () => request.destroy(new Error('Audio receive timeout')));
            request.on('error', reject);
            request.on('close', resolve);
        });
    }

    async syncCall() {
        if (this.stopped) return;
        if (this.callId) {
            const current = await this.prisma.call.findUnique({ where: { id: this.callId } });
            if (!current || ['ended', 'missed', 'interrupted'].includes(current.state)) await this.finalizeCall(current);
            return;
        }
        const active = await this.prisma.call.findFirst({
            where: { deviceId: this.device.id, state: 'active' },
            orderBy: { answered_at: 'asc' }
        });
        if (active) await this.startCall(active);
    }

    async startCall(call) {
        if (this.writer || this.stopped) return;
        const filePath = call.original_path || safeCallPath(call.id, 'original');
        const exists = fs.existsSync(filePath) && fs.statSync(filePath).size >= 44;
        this.writer = new WavAppender(filePath, call.input_sample_rate || this.device.input_sample_rate, exists);
        this.callId = call.id;
        let boundaryMode = 'event_received_with_preroll';
        let boundaryIncomplete = false;
        if (!exists) {
            let preRoll = this.ring.snapshot();
            if (call.answered_sample_counter && this.nextSampleCounter !== null) {
                const answered = BigInt(call.answered_sample_counter);
                const wantedStart = answered - BigInt(Math.floor(this.device.input_sample_rate * this.preRollMs / 1000));
                const availableStart = this.nextSampleCounter - BigInt(preRoll.length / 2);
                const actualStart = wantedStart > availableStart ? wantedStart : availableStart;
                const offsetSamples = actualStart > availableStart ? Number(actualStart - availableStart) : 0;
                preRoll = preRoll.subarray(Math.min(preRoll.length, offsetSamples * 2));
                this.writerStartCounter = actualStart;
                boundaryMode = wantedStart < availableStart ? 'sample_counter_partial_preroll' : 'sample_counter_with_preroll';
                boundaryIncomplete = answered < availableStart || answered > this.nextSampleCounter;
            }
            this.writer.append(preRoll);
        }
        await this.prisma.call.update({
            where: { id: call.id },
            data: {
                original_path: filePath,
                recording_status: 'recording',
                recording_incomplete: call.recording_incomplete || exists || boundaryIncomplete,
                boundary_mode: boundaryMode,
            }
        });
    }

    async finalizeCall(call) {
        if (!this.writer) { this.callId = null; return; }
        if (call?.ended_sample_counter && this.writerStartCounter !== null && !this.counterGap) {
            const wantedSamples = BigInt(call.ended_sample_counter) - this.writerStartCounter;
            if (wantedSamples >= 0n && wantedSamples <= BigInt(Number.MAX_SAFE_INTEGER)) this.writer.truncateData(Number(wantedSamples) * 2);
        }
        const result = this.writer.finalize();
        this.writer = null;
        const callId = this.callId;
        this.callId = null;
        this.writerStartCounter = null;
        this.counterGap = false;
        this.limitReached = false;
        await this.prisma.call.update({
            where: { id: callId },
            data: {
                recording_status: call?.recording_incomplete ? 'incomplete' : 'ready',
                sample_count: result.sampleCount,
                audio_bytes: result.dataBytes,
                audio_duration_ms: Math.round(result.sampleCount / this.device.input_sample_rate * 1000),
                processing_status: result.dataBytes ? 'queued' : 'skipped',
                transcription_status: result.dataBytes ? 'waiting' : 'skipped',
            }
        });
        if (result.dataBytes) {
            await this.prisma.processingJob.upsert({
                where: { callId_type_version: { callId, type: 'process', version: '1' } },
                update: { status: 'queued', run_after: new Date(), locked_by: null, locked_at: null },
                create: { callId, type: 'process', version: '1' },
            });
        }
    }

    async markLimitExceeded() {
        if (!this.callId) return;
        if (this.limitReached) return;
        this.limitReached = true;
        await this.prisma.call.update({ where: { id: this.callId }, data: { recording_incomplete: true, recording_error: 'Recording stopped at configured maximum file size' } });
    }

    async noteDisconnect(error) {
        if (this.callId) {
            const call = await this.prisma.call.findUnique({ where: { id: this.callId } });
            const segments = call?.missing_segments_json ? JSON.parse(call.missing_segments_json) : [];
            segments.push({ detected_at: new Date().toISOString(), reason: error.message });
            await this.prisma.call.update({ where: { id: this.callId }, data: { recording_incomplete: true, missing_segments_json: JSON.stringify(segments) } });
        }
        await this.setStatus(error.code === 'STREAM_CONFLICT' ? 'conflict' : 'reconnecting', error.message);
    }

    async renewLease() {
        const result = await this.prisma.device.updateMany({
            where: { id: this.device.id, active: true, receiver_lease_owner: this.owner },
            data: { receiver_lease_until: new Date(Date.now() + config.receiverLeaseMs) }
        });
        if (!result.count) this.stop();
    }

    async setStatus(status, error = null, connected = false) {
        await this.prisma.device.updateMany({
            where: { id: this.device.id, receiver_lease_owner: this.owner },
            data: {
                receiver_status: status,
                receiver_error: error,
                ...(connected ? { receiver_connected_at: new Date(), stream_session_id: this.device.stream_session_id } : {}),
            }
        });
    }

    throttledAudioHeartbeat() {
        const now = Date.now();
        if (this.lastHeartbeat && now - this.lastHeartbeat < 2000) return;
        this.lastHeartbeat = now;
        this.prisma.device.updateMany({
            where: { id: this.device.id, receiver_lease_owner: this.owner },
            data: { receiver_last_audio_at: new Date() }
        }).catch(() => {});
    }

    async setError(message) { await this.setStatus('error', message); }

    stop() {
        if (this.stopped) return;
        this.stopped = true;
        clearInterval(this.callPoll);
        clearInterval(this.leaseTimer);
        if (this.request) this.request.destroy();
        if (this.writer) this.writer.close();
    }
}

class ReceiverManager {
    constructor(prisma) {
        this.prisma = prisma;
        this.owner = `${process.pid}-${crypto.randomUUID()}`;
        this.sessions = new Map();
    }

    async start() {
        const stale = new Date(Date.now() - config.receiverLeaseMs * 2);
        await this.prisma.processingJob.updateMany({ where: { status: 'running', locked_at: { lt: stale } }, data: { status: 'queued', locked_by: null, locked_at: null } });
        await this.sync();
        this.timer = setInterval(() => this.sync().catch(error => console.error('Receiver sync failed:', error)), config.receiverSyncMs);
    }

    async sync() {
        const devices = await this.prisma.device.findMany({ where: { active: true, deleted_at: null } });
        const wanted = new Set(devices.map(device => device.id));
        for (const [id, session] of this.sessions) {
            const current = devices.find(device => device.id === id);
            const changed = current && (current.ip_address !== session.device.ip_address || current.input_sample_rate !== session.device.input_sample_rate || current.audio_format !== session.device.audio_format || current.audio_channels !== session.device.audio_channels);
            if (!wanted.has(id) || changed) {
                session.stop();
                this.sessions.delete(id);
                await this.prisma.device.updateMany({ where: { id, receiver_lease_owner: this.owner }, data: { receiver_lease_owner: null, receiver_lease_until: null, receiver_status: current?.active ? 'connecting' : 'stopped' } });
            }
        }
        const policy = await this.prisma.recordingPolicy.upsert({ where: { id: 'default' }, update: {}, create: { id: 'default' } });
        for (const device of devices) {
            if (this.sessions.has(device.id)) continue;
            if (!device.ip_address || !device.input_sample_rate || device.audio_format !== 'pcm_s16le' || device.audio_channels !== 1) {
                await this.prisma.device.update({ where: { id: device.id }, data: { receiver_status: 'configuration_required', receiver_error: !device.input_sample_rate ? 'Input sample rate must be declared or experimentally calibrated' : 'Only mono pcm_s16le is currently supported' } });
                continue;
            }
            const acquired = await this.prisma.device.updateMany({
                where: {
                    id: device.id,
                    active: true,
                    OR: [
                        { receiver_lease_owner: this.owner },
                        { receiver_lease_owner: null },
                        { receiver_lease_until: { lt: new Date() } },
                    ]
                },
                data: { receiver_lease_owner: this.owner, receiver_lease_until: new Date(Date.now() + config.receiverLeaseMs), receiver_status: 'connecting', receiver_error: null }
            });
            if (!acquired.count) continue;
            const session = new DeviceReceiver(this.prisma, device, this.owner, { preRollMs: policy.pre_roll_ms, maxCallBytes: policy.max_call_bytes });
            this.sessions.set(device.id, session);
            session.run().finally(() => this.sessions.delete(device.id));
        }
    }

    async stop() {
        clearInterval(this.timer);
        for (const session of this.sessions.values()) session.stop();
        this.sessions.clear();
        await this.prisma.device.updateMany({ where: { receiver_lease_owner: this.owner }, data: { receiver_lease_owner: null, receiver_lease_until: null, receiver_status: 'stopped' } });
    }
}

module.exports = { DeviceReceiver, ReceiverManager };
