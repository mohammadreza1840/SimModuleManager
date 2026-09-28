const express = require('express');
const config = require('../config');
const { adminAuth, hashToken, safeEqual } = require('../middleware/auth');

const router = express.Router();

function registrationAuthorized(req) {
    if (!config.registrationToken) return process.env.NODE_ENV !== 'production';
    const supplied = (req.get('authorization') || '').replace(/^Bearer /, '');
    return supplied && safeEqual(supplied, config.registrationToken);
}

router.post('/register', async (req, res) => {
    const { iccid, ip, phone } = req.body;
    if (!iccid) return res.status(400).json({ error: 'iccid is required' });
    const explicitDeviceId = req.body.device_id || req.body.deviceId;
    const deviceId = String(explicitDeviceId || `legacy-${iccid}`);
    if (!/^[a-zA-Z0-9._:-]{3,128}$/.test(deviceId)) return res.status(400).json({ error: 'device_id has an invalid format' });
    const authorized = registrationAuthorized(req);
    if (config.registrationToken && !authorized) return res.status(401).json({ error: 'Invalid device registration credential' });
    const declaredRate = Number(req.body.audio?.sample_rate || req.body.sample_rate || 0) || null;
    if (declaredRate && (declaredRate < 4000 || declaredRate > 192000)) return res.status(400).json({ error: 'sample_rate is outside the accepted range' });

    try {
        const existing = await req.prisma.device.findUnique({ where: { id: deviceId } });
        const suppliedDeviceToken = req.body.device_token;
        const maySetCredential = authorized && suppliedDeviceToken && (!existing?.device_key_hash || req.body.rotate_device_key === true);
        const eventsSupported = Boolean(explicitDeviceId && (existing?.device_key_hash || maySetCredential) && Number(req.body.call_events_version) >= 1);
        const device = await req.prisma.$transaction(async tx => {
            const saved = await tx.device.upsert({
                where: { id: deviceId },
                update: {
                    ip_address: ip || existing?.ip_address || null,
                    label: req.body.label || existing?.label || null,
                    active: true,
                    deleted_at: null,
                    firmware_events_supported: eventsSupported || existing?.firmware_events_supported || false,
                    ...(declaredRate ? { input_sample_rate: declaredRate, input_rate_source: 'firmware', experimental_calibration: false } : {}),
                    ...(maySetCredential ? { device_key_hash: hashToken(suppliedDeviceToken) } : {}),
                },
                create: {
                    id: deviceId,
                    ip_address: ip || null,
                    label: req.body.label || null,
                    firmware_events_supported: eventsSupported,
                    input_sample_rate: declaredRate,
                    input_rate_source: 'firmware',
                    device_key_hash: maySetCredential ? hashToken(suppliedDeviceToken) : null,
                }
            });
            await tx.simcard.updateMany({ where: { deviceId, NOT: { iccid: String(iccid) } }, data: { deviceId: null } });
            await tx.simcard.upsert({
                where: { iccid: String(iccid) },
                update: { ip_address: ip || null, phone_number: phone || null, last_seen: new Date(), status: 'online', deviceId },
                create: { iccid: String(iccid), ip_address: ip || null, phone_number: phone || null, last_seen: new Date(), status: 'online', deviceId }
            });
            return saved;
        });
        res.json({
            success: true,
            device_id: device.id,
            legacy_identity: !explicitDeviceId,
            call_events_ready: device.firmware_events_supported,
            event_endpoint: `/api/device-events/${encodeURIComponent(device.id)}/events`,
            warning: explicitDeviceId ? undefined : 'Legacy registration accepted, but stable device identity and authenticated call events are not available.',
        });
    } catch (error) {
        console.error('Error registering device:', error);
        res.status(500).json({ error: 'Internal server error' });
    }
});

router.use(adminAuth);

router.get('/', async (req, res) => {
    const where = { deleted_at: null, ...(req.auth.deviceIds ? { id: { in: req.auth.deviceIds } } : {}) };
    const devices = await req.prisma.device.findMany({ where, include: { simcard: true }, orderBy: { updated_at: 'desc' } });
    res.json(devices.map(({ device_key_hash, ...device }) => ({ ...device, credential_configured: Boolean(device_key_hash) })));
});

router.get('/:deviceId/live-audio', async (req, res) => {
    if (req.auth.role !== 'admin' && req.auth.deviceIds && !req.auth.deviceIds.includes(req.params.deviceId)) return res.status(403).json({ error: 'Device access denied' });
    
    const receiverManager = req.app.get('receiverManager');
    if (!receiverManager) return res.status(503).json({ error: 'Receiver service is unavailable' });

    const receiver = receiverManager.sessions.get(req.params.deviceId);
    if (!receiver || !receiver.device) return res.status(404).json({ error: 'Device audio stream is not active' });

    res.setHeader('Content-Type', 'audio/wav');
    res.setHeader('Transfer-Encoding', 'chunked');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('Cache-Control', 'no-store');

    const sampleRate = receiver.device.input_sample_rate || 44240;
    const header = Buffer.alloc(44);
    header.write('RIFF', 0);
    header.writeUInt32LE(0xFFFFFFFF, 4);
    header.write('WAVE', 8);
    header.write('fmt ', 12);
    header.writeUInt32LE(16, 16);
    header.writeUInt16LE(1, 20);
    header.writeUInt16LE(1, 22);
    header.writeUInt32LE(sampleRate, 24);
    header.writeUInt32LE(sampleRate * 2, 28);
    header.writeUInt16LE(2, 32);
    header.writeUInt16LE(16, 34);
    header.write('data', 36);
    header.writeUInt32LE(0xFFFFFFFF, 40);

    res.write(header);

    const onAudio = (pcmChunk) => {
        const buf = Buffer.from(pcmChunk.buffer, pcmChunk.byteOffset, pcmChunk.byteLength);
        res.write(buf);
    };

    receiver.on('audio', onAudio);

    req.on('close', () => {
        receiver.off('audio', onAudio);
    });
});

router.patch('/:deviceId', async (req, res) => {
    if (req.auth.role !== 'admin' && req.auth.deviceIds && !req.auth.deviceIds.includes(req.params.deviceId)) return res.status(403).json({ error: 'Device access denied' });
    const allowed = ['label', 'ip_address', 'active', 'input_sample_rate', 'output_sample_rate', 'audio_profile'];
    const data = {};
    for (const key of allowed) if (req.body[key] !== undefined) data[key] = req.body[key];
    if (data.input_sample_rate !== undefined) {
        const rate = Number(data.input_sample_rate);
        if (!Number.isInteger(rate) || rate < 4000 || rate > 192000) return res.status(400).json({ error: 'input_sample_rate must be an integer from 4000 to 192000' });
        data.input_sample_rate = rate;
        data.input_rate_source = req.body.experimental_calibration ? 'manual_experimental' : 'manual';
        data.experimental_calibration = Boolean(req.body.experimental_calibration);
    }
    if (data.output_sample_rate !== undefined && ![8000, 16000, 24000, 32000, 48000].includes(Number(data.output_sample_rate))) return res.status(400).json({ error: 'Unsupported output sample rate' });
    if (data.output_sample_rate !== undefined) data.output_sample_rate = Number(data.output_sample_rate);
    if (data.audio_profile && !['raw', 'mild', 'strong'].includes(data.audio_profile)) return res.status(400).json({ error: 'Unsupported audio profile' });
    if (data.active === false) {
        await req.prisma.call.updateMany({ where: { deviceId: req.params.deviceId, state: 'active' }, data: { state: 'interrupted', ended_at: new Date(), end_reason: 'receiver_disabled', recording_incomplete: true } });
        Object.assign(data, { receiver_status: 'stopped', receiver_lease_owner: null, receiver_lease_until: null });
    }
    try {
        const updated = await req.prisma.device.update({ where: { id: req.params.deviceId }, data });
        const { device_key_hash, ...safe } = updated;
        res.json(safe);
    } catch { res.status(404).json({ error: 'Device not found' }); }
});

router.delete('/:deviceId', async (req, res) => {
    if (req.auth.role !== 'admin') return res.status(403).json({ error: 'Admin role required' });
    try {
        await req.prisma.$transaction([
            req.prisma.simcard.updateMany({ where: { deviceId: req.params.deviceId }, data: { deviceId: null } }),
            req.prisma.call.updateMany({ where: { deviceId: req.params.deviceId, state: 'active' }, data: { state: 'interrupted', ended_at: new Date(), end_reason: 'device_deleted', recording_incomplete: true } }),
            req.prisma.device.update({ where: { id: req.params.deviceId }, data: { active: false, deleted_at: new Date(), ip_address: null, device_key_hash: null, receiver_status: 'stopped', receiver_lease_owner: null, receiver_lease_until: null } }),
        ]);
        res.status(204).end();
    } catch { res.status(404).json({ error: 'Device not found' }); }
});

module.exports = router;
