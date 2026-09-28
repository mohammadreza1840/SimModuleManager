import React, { useCallback, useEffect, useState } from 'react';
import { AlertTriangle, PhoneCall } from 'lucide-react';
import { api, API_BASE } from '../api';
import CallDetailModal from './CallDetailModal';

const stateLabel = { ringing: 'در حال زنگ', active: 'فعال', ended: 'پایان‌یافته', missed: 'بی‌پاسخ', interrupted: 'قطع‌شده' };

export default function CallsPanel({ devices }) {
  const [calls, setCalls] = useState([]);
  const [selected, setSelected] = useState(null);
  const [filters, setFilters] = useState({ device_id: '', iccid: '', number: '', state: '', from: '', to: '' });
  const [error, setError] = useState('');
  
  const token = sessionStorage.getItem('simcard_api_token') || import.meta.env.VITE_API_TOKEN || '';
  const tokenQuery = token ? `?token=${encodeURIComponent(token)}` : '';

  const load = useCallback(async () => {
    try {
      const params = Object.fromEntries(Object.entries(filters).filter(([, value]) => value));
      const { data } = await api.get('/calls', { params }); setCalls(data.data); setError('');
    } catch (e) { setError(e.response?.data?.error || 'دریافت تماس‌ها ناموفق بود.'); }
  }, [filters]);
  
  useEffect(() => { const initial = setTimeout(load, 0); const timer = setInterval(load, 5000); return () => { clearTimeout(initial); clearInterval(timer); }; }, [load]);
  
  return (
    <div dir="rtl">
      <div className="receiver-grid">
        {devices.map(device => (
          <div className="receiver-card" key={device.id}>
            <div>
              <strong>{device.label || device.id}</strong>
              <small>{device.simcard?.phone_number || device.simcard?.iccid || 'بدون سیم‌کارت'}</small>
            </div>
            <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', alignItems: 'flex-start', marginTop: '10px' }}>
              <span className={`receiver-state state-${device.receiver_status}`}>{device.receiver_status}</span>
              {device.receiver_status === 'connected' && (
                <audio 
                  src={`${API_BASE}/devices/${device.id}/live-audio${tokenQuery}`}
                  controls 
                  style={{ height: '32px', width: '100%' }}
                  title="پخش زنده استریم"
                />
              )}
            </div>
            {!device.firmware_events_supported && <small className="not-ready">چرخه تماس firmware آماده نیست</small>}
          </div>
        ))}
      </div>
      <div className="filters">
        <select className="form-input" value={filters.device_id} onChange={e => setFilters({ ...filters, device_id: e.target.value })}><option value="">همه دستگاه‌ها</option>{devices.map(d => <option key={d.id} value={d.id}>{d.label || d.id}</option>)}</select>
        <input className="form-input" placeholder="ICCID" value={filters.iccid} onChange={e => setFilters({ ...filters, iccid: e.target.value })}/>
        <input className="form-input" placeholder="شماره تماس‌گیرنده" value={filters.number} onChange={e => setFilters({ ...filters, number: e.target.value })}/>
        <select className="form-input" value={filters.state} onChange={e => setFilters({ ...filters, state: e.target.value })}><option value="">همه وضعیت‌ها</option>{Object.entries(stateLabel).map(([value, label]) => <option key={value} value={value}>{label}</option>)}</select>
        <input className="form-input" type="date" value={filters.from} onChange={e => setFilters({ ...filters, from: e.target.value })}/>
        <input className="form-input" type="date" value={filters.to} onChange={e => setFilters({ ...filters, to: `${e.target.value}T23:59:59` })}/>
      </div>
      {error && <div className="inline-error">{error}</div>}
      <div className="table-wrapper"><table><thead><tr><th>زمان</th><th>دستگاه / سیم‌کارت</th><th>تماس‌گیرنده</th><th>مدت</th><th>تماس</th><th>صوت</th><th>متن</th></tr></thead><tbody>
        {!calls.length ? <tr><td colSpan="7" className="empty"><PhoneCall/> تماسی ثبت نشده است.</td></tr> : calls.map(call => <tr key={call.id} onClick={() => setSelected(call.id)} className="clickable-row"><td>{new Date(call.ringing_at || call.created_at).toLocaleString('fa-IR')}</td><td>{call.device?.label || call.deviceId}<small className="table-sub">{call.sim_iccid_snapshot || 'نامشخص'}</small></td><td dir="ltr">{call.caller_number || 'Unknown'}</td><td>{Math.round((call.duration_ms || 0) / 1000)}s</td><td>{stateLabel[call.state] || call.state}</td><td>{call.recording_incomplete && <AlertTriangle className="warn-icon" size={16}/>} {call.recording_status}</td><td>{call.transcription_status}</td></tr>)}
      </tbody></table></div>
      {selected && <CallDetailModal callId={selected} onClose={() => setSelected(null)} onChanged={load}/>} 
    </div>
  );
}
