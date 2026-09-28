const crypto = require('node:crypto');
const config = require('../config');

function hashToken(token) {
    return crypto.createHash('sha256').update(String(token)).digest('hex');
}

function bearer(req) {
    const value = req.get('authorization') || '';
    if (value.startsWith('Bearer ')) return value.slice(7);
    if (req.query.token) return req.query.token;
    return '';
}

function safeEqual(a, b) {
    const left = Buffer.from(String(a));
    const right = Buffer.from(String(b));
    return left.length === right.length && crypto.timingSafeEqual(left, right);
}

function configuredTokens() {
    const tokens = new Map();
    if (config.adminApiToken) tokens.set(config.adminApiToken, { role: 'admin', deviceIds: null });
    if (config.apiTokensJson) {
        try {
            const parsed = JSON.parse(config.apiTokensJson);
            for (const [token, scope] of Object.entries(parsed)) tokens.set(token, scope || {});
        } catch {
            throw new Error('API_TOKENS_JSON must be valid JSON');
        }
    }
    return tokens;
}

function adminAuth(req, res, next) {
    const tokens = configuredTokens();
    if (tokens.size === 0 && process.env.NODE_ENV !== 'production') {
        req.auth = { role: 'admin', deviceIds: null, insecureDevelopmentMode: true };
        return next();
    }
    const token = bearer(req);
    for (const [candidate, scope] of tokens) {
        if (safeEqual(token, candidate)) {
            req.auth = { role: scope.role || 'viewer', deviceIds: scope.deviceIds || null };
            return next();
        }
    }
    return res.status(401).json({ error: 'Authentication required' });
}

function canAccessDevice(req, deviceId) {
    return req.auth?.role === 'admin' || req.auth?.deviceIds === null || req.auth?.deviceIds?.includes(deviceId);
}

function requireDeviceAccess(getDeviceId) {
    return (req, res, next) => {
        const deviceId = getDeviceId(req);
        if (!canAccessDevice(req, deviceId)) return res.status(403).json({ error: 'Device access denied' });
        next();
    };
}

async function deviceAuth(req, res, next) {
    const device = await req.prisma.device.findUnique({ where: { id: req.params.deviceId } });
    const token = bearer(req);
    if (!device || !device.device_key_hash || !token || !safeEqual(hashToken(token), device.device_key_hash)) {
        return res.status(401).json({ error: 'Invalid device credential' });
    }
    req.device = device;
    next();
}

module.exports = { adminAuth, canAccessDevice, requireDeviceAccess, deviceAuth, hashToken, bearer, safeEqual };
