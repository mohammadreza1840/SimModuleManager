const path = require('node:path');
require('dotenv').config({ path: path.resolve(__dirname, '..', '.env') });

const express = require('express');
const cors = require('cors');
const { PrismaClient } = require('@prisma/client');

const deviceRoutes = require('./routes/device');
const simcardRoutes = require('./routes/simcard');
const deviceEventRoutes = require('./routes/deviceEvents');
const callRoutes = require('./routes/calls');
const settingsRoutes = require('./routes/settings');
const webhookRoutes = require('./routes/webhook');
const { adminAuth } = require('./middleware/auth');
const { startHealthCheckDaemon } = require('./services/healthCheck');
const { ReceiverManager } = require('./services/receiver');
const { JobWorker } = require('./services/jobWorker');
const { startRetentionDaemon } = require('./services/retention');
const { ensureStorage } = require('./services/storage');

function createApp(prisma) {
    const app = express();
    app.use(cors({ origin: process.env.CORS_ORIGIN ? process.env.CORS_ORIGIN.split(',') : true }));
    app.use(express.json({ limit: '256kb' }));
    app.use((req, _res, next) => { req.prisma = prisma; next(); });
    app.get('/healthz', (_req, res) => res.json({ status: 'ok' }));
    app.use('/api/devices', deviceRoutes);
    app.use('/api/device-events', deviceEventRoutes);
    app.use('/api/simcards', adminAuth, simcardRoutes);
    app.use('/api/calls', adminAuth, callRoutes);
    app.use('/api/settings', adminAuth, settingsRoutes);
    app.use('/webhook', webhookRoutes);
    app.use((error, _req, res, _next) => {
        console.error(error);
        res.status(500).json({ error: 'Internal server error' });
    });
    return app;
}

function listen(app, port) {
    return new Promise((resolve, reject) => {
        const server = app.listen(port, '0.0.0.0');
        const onError = error => reject(error);
        server.once('error', onError);
        server.once('listening', () => {
            server.off('error', onError);
            console.log(`Backend server listening on port ${port}`);
            resolve(server);
        });
    });
}

async function start() {
    ensureStorage();
    const prisma = new PrismaClient();
    await prisma.$connect();
    const app = createApp(prisma);
    const port = process.env.PORT || 3001;
    let server;
    try {
        // Do not start receiver/job daemons unless this process owns the HTTP
        // port. Otherwise an old server and a new background worker can process
        // the same audio using different in-memory processing versions.
        server = await listen(app, port);
    } catch (error) {
        await prisma.$disconnect();
        throw error;
    }
    const receiver = new ReceiverManager(prisma);
    app.set('receiverManager', receiver);
    const jobs = new JobWorker(prisma);
    const health = startHealthCheckDaemon(prisma);
    const retention = startRetentionDaemon(prisma);
    await Promise.all([receiver.start(), jobs.start()]);

    async function shutdown() {
        server.close();
        health?.();
        retention?.();
        jobs.stop();
        await receiver.stop();
        await prisma.$disconnect();
    }
    process.once('SIGINT', shutdown);
    process.once('SIGTERM', shutdown);
    return { app, server, prisma, receiver, jobs, shutdown };
}

if (require.main === module) start().catch(error => { console.error(error); process.exitCode = 1; });

module.exports = { createApp, listen, start };
