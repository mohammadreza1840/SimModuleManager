import React, { useCallback, useEffect, useState } from 'react';
import { Copy, RefreshCw, Search, X } from 'lucide-react';
import { api, API_BASE } from '../api';
import AudioAsset from './AudioAsset';

const labels = { ready: 'آماده', recording: 'در حال ضبط', queued: 'در صف', processing: 'در حال پردازش', transcribing: 'در حال تبدیل', failed: 'ناموفق', waiting: 'منتظر', incomplete: 'ناقص', skipped: 'رد شده', no_signal: 'بدون سیگنال' };
const profileLabels = { raw: 'خام', mild: 'شفاف‌سازی متعادل', strong: 'شفاف‌سازی قوی' };

export default function CallDetailModal({ callId, onClose, onChanged }) {
  const [call, setCall] = useState(null);
  const [search, setSearch] = useState('');
  
  const token = sessionStorage.getItem('simcard_api_token') || import.meta.env.VITE_API_TOKEN || '';
  const tokenQuery = token ? `?token=${encodeURIComponent(token)}` : '';

  const load = useCallback(() => api.get(`/calls/${callId}`).then(({ data }) => setCall(data)), [callId]);
  useEffect(() => { const initial = setTimeout(load, 0); const timer = setInterval(load, 3000); return () => { clearTimeout(initial); clearInterval(timer); }; }, [load]);
  const transcript = (() => {
    if (!call?.transcript || !search) return call?.transcript;
    return call.transcript.split(new RegExp(`(${search.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')})`, 'gi')).map((part, i) => part.toLowerCase() === search.toLowerCase() ? <mark key={i}>{part}</mark> : part);
  })();
  const retry = async (type, profile) => { await api.post(`/calls/${callId}/retry`, { type, profile }); await load(); onChanged(); };
  if (!call) return null;
  return (
    <div className="modal-overlay" onMouseDown={e => e.target === e.currentTarget && onClose()}>
      <div className="modal-content glass-panel call-detail" dir="rtl">
        <div className="modal-header"><h2>جزئیات تماس</h2><button className="modal-close" onClick={onClose}><X /></button></div>
        {call.recording_incomplete && <div className="warning-banner">این ضبط ناقص است. {call.missing_segments?.length || 0} وقفهٔ دریافت ثبت شده و صدای ازدست‌رفته قابل بازیابی فرض نشده است.</div>}
        <div className="detail-grid">
          <div><span>دستگاه</span><b>{call.device?.label || call.deviceId}</b></div>
          <div><span>سیم‌کارت هنگام تماس</span><b>{call.sim_iccid_snapshot || 'نامشخص'}</b></div>
          <div><span>تماس‌گیرنده</span><b dir="ltr">{call.caller_number || 'نامشخص'}</b></div>
          <div><span>مدت تماس / صوت</span><b>{Math.round((call.duration_ms || 0) / 1000)} / {Math.round((call.audio_duration_ms || 0) / 1000)} ثانیه</b></div>
          <div><span>وضعیت صوت اصلی</span><b>{labels[call.recording_status] || call.recording_status}</b></div>
          <div><span>پردازش / متن</span><b>{labels[call.processing_status] || call.processing_status} / {labels[call.transcription_status] || call.transcription_status}</b></div>
          <div><span>نرخ ورودی / خروجی</span><b>{call.input_sample_rate || '؟'} / {call.output_sample_rate || '؟'} Hz</b></div>
          <div><span>پروفایل</span><b>{profileLabels[call.audio_profile] || profileLabels.mild}</b></div>
        </div>
        
        {(call.state === 'active' || call.state === 'ringing') && (
          <div className="live-stream-player" style={{ marginTop: '20px', background: 'rgba(255, 68, 68, 0.1)', padding: '12px', borderRadius: '8px', border: '1px solid rgba(255, 68, 68, 0.3)' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: '8px', marginBottom: '8px' }}>
              <span style={{ width: '10px', height: '10px', background: '#ff4444', borderRadius: '50%', display: 'inline-block', boxShadow: '0 0 8px #ff4444' }}></span>
              <strong style={{ color: '#ff4444' }}>پخش زنده مکالمه</strong>
            </div>
            <audio 
               src={`${API_BASE}/devices/${call.deviceId}/live-audio${tokenQuery}`}
               controls 
               autoPlay
               style={{ height: '36px', width: '100%' }}
               title="پخش زنده تماس"
            />
          </div>
        )}

        {call.original_available && <AudioAsset callId={call.id} variant="original" label="فایل اصلی" />}
        {call.processed_available && <AudioAsset callId={call.id} variant="processed" label="نسخهٔ پردازش‌شده"
          revision={`${call.processing_version || ''}:${call.processing_attempts}:${call.updated_at}`} />}
        {(call.processing_error || call.transcription_error) && <div className="inline-error">{call.processing_error || call.transcription_error}</div>}
        <div className="retry-row">
          <button className="btn btn-secondary" onClick={() => retry('process', call.audio_profile)}><RefreshCw size={15}/> پردازش دوباره</button>
          <button className="btn btn-secondary" onClick={() => retry('transcribe')} disabled={!call.original_available}><RefreshCw size={15}/> ترنسکرایب دوباره</button>
          <select className="form-input compact" value={call.audio_profile || 'mild'} onChange={e => retry('process', e.target.value)}><option value="raw">خام (بدون حذف نویز)</option><option value="mild">شفاف‌سازی متعادل</option><option value="strong">شفاف‌سازی قوی</option></select>
        </div>
        <div className="transcript-header"><h3>متن مکالمه</h3><div className="search-box"><Search size={15}/><input value={search} onChange={e => setSearch(e.target.value)} placeholder="جست‌وجو" /></div><button className="btn btn-secondary" disabled={!call.transcript} onClick={() => navigator.clipboard.writeText(call.transcript)}><Copy size={15}/> کپی</button></div>
        <div className="transcript" lang="fa" dir="rtl">{transcript || (call.transcription_status === 'ready' ? 'متنی بازگردانده نشد.' : 'متن هنوز آماده نیست.')}</div>
        <p className="footnote">این ورودی خروجی بلندگوی مودم است؛ ضبط کامل دو طرف یا جداسازی گویندگان تأیید نشده است.</p>
      </div>
    </div>
  );
}
