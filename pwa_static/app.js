// SSE client. Server pushes a state event when anything changes plus a
// keepalive comment every 10s — no client polling. Control writes are
// still plain POSTs.
const $ = id => document.getElementById(id);
let lastSetpoint = null;
let es = null;

function setConn(ok) {
  const c = $('conn');
  c.textContent = ok ? 'live' : 'offline';
  c.className = ok ? '' : 'bad';
}

function render(s) {
  $('temp').textContent = s.indoor_temp > 0 ? s.indoor_temp.toFixed(1) + ' °C' : '--.- °C';
  $('sp').textContent   = 'setpoint ' + s.setpoint.toFixed(1) + ' °C';
  lastSetpoint = s.setpoint;
  $('mod').textContent  = (s.modulation_level || 0).toFixed(0) + '%';
  $('bo').textContent   = s.boiler_out_temp > 0 ? s.boiler_out_temp.toFixed(1) + '°C' : '--';
  $('br').textContent   = s.boiler_in_temp  > 0 ? s.boiler_in_temp.toFixed(1)  + '°C' : '--';
  $('chsp').textContent = s.ch_setpoint     > 0 ? s.ch_setpoint.toFixed(1)     + '°C' : '--';
  $('wp').textContent   = s.water_pressure  > 0 ? s.water_pressure.toFixed(2)  + ' bar' : '--';
  $('hum').textContent  = s.humidity        > 0 ? s.humidity.toFixed(0)        + '%'   : '--';
  $('eco2').textContent = s.eco2  > 0 ? s.eco2  + ' ppm' : '--';
  $('tvoc').textContent = s.tvoc  > 0 ? s.tvoc  + ' ppb' : '--';

  let b = '<span class="badge ' + (s.burner_on ? 'fire' : 'idle') + '">'
        + (s.burner_on ? 'CV brandt' : 'CV uit') + '</span>';
  if (s.dhw_on) b += '<span class="badge dhw">warm water</span>';
  $('badges').innerHTML = b;

  // Curtains come via Home Assistant; only show the card when a curtain
  // device is actually reporting (otherwise it's an empty "  (0%)" card on
  // Toons without curtains / with HA offline).
  const hasCurtain = s.curtain_state && s.curtain_state.length > 0;
  $('card-curtains').style.display = hasCurtain ? '' : 'none';
  if (hasCurtain) {
    $('ct-st').textContent  = s.curtain_state + ' (' + s.curtain_pos + '%)';
    $('ct-bat').textContent = 'bat ' + s.curtain_battery + '%';
  }

  document.querySelectorAll('.progs button').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.p, 10) === s.program_state);
  });
}

function connect() {
  if (es) { try { es.close(); } catch (_) {} }
  es = new EventSource('/api/state/stream');
  es.onopen    = () => setConn(true);
  es.onerror   = () => {
    setConn(false);
    // EventSource auto-reconnects, but force a fresh handshake after a beat
    // so the server's heartbeat-failure path triggers a clean reconnect.
    setTimeout(connect, 3000);
  };
  es.onmessage = ev => {
    try { render(JSON.parse(ev.data)); setConn(true); }
    catch (e) { /* ignore malformed frame */ }
  };
}

async function setSetpoint(v) {
  const r = await fetch('/api/setpoint', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ value: v.toFixed(2) })
  });
  if (r.ok) $('sp').textContent = 'setpoint ' + v.toFixed(1) + ' °C (sending…)';
}

$('inc').onclick = () => setSetpoint((lastSetpoint || 18) + 0.5);
$('dec').onclick = () => setSetpoint((lastSetpoint || 18) - 0.5);

document.querySelectorAll('.progs button').forEach(b => {
  b.onclick = () => fetch('/api/program', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({ state: parseInt(b.dataset.p, 10) })
  });
});

window.curtain = a => fetch('/api/curtain', {
  method: 'POST',
  headers: {'Content-Type': 'application/json'},
  body: JSON.stringify({ action: a })
});

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('/sw.js').catch(() => {});
}

connect();

/* -------------------- Pakketten -------------------- */
function escapeHtml(s) {
  return String(s||'').replace(/[&<>"']/g,
    c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

/* Resolve a tracking URL with the package's postal_code substituted in.
 * Supports three patterns commonly seen in carrier URLs:
 *   1. {pc}            literal placeholder
 *   2. (postal code)   user-typed reminder
 *   3. trailing slash  appends `<postcode>` (e.g. parcel.trunkrs.nl/<id>/)
 * Returns the unchanged URL if nothing matches or there's no postal code. */
function resolveTrackUrl(url, pc) {
  if (!url) return '';
  if (!pc)  return url;
  if (url.includes('{pc}'))            return url.replace('{pc}', encodeURIComponent(pc));
  if (url.match(/\(postal\s*code\)/i)) return url.replace(/\(postal\s*code\)/i, encodeURIComponent(pc));
  if (url.endsWith('/'))               return url + encodeURIComponent(pc);
  return url;
}

const PKG = {
  fmtDate(s) {
    if (!s) return '';
    const d = new Date(s + 'T00:00:00');
    const t = new Date(); t.setHours(0,0,0,0);
    const dd = Math.round((d - t) / 86400000);
    if (dd === 0)  return 'vandaag';
    if (dd === 1)  return 'morgen';
    if (dd === -1) return 'gisteren';
    if (dd > 1 && dd < 7) return `over ${dd} d`;
    return d.toLocaleDateString('nl-NL', { day:'2-digit', month:'short' });
  },
  render(list) {
    const pending  = list.filter(p => p.status === 'pending');
    const received = list.filter(p => p.status === 'received');
    const today = new Date().toISOString().slice(0,10);
    $('pkg-count').textContent = pending.length
      ? `(${pending.length} open)` : '';
    const el = $('pkg-list');
    if (!list.length) {
      el.innerHTML = '<div style="color:#5a7895;font-size:13px">Geen pakketten</div>';
      return;
    }
    const card = p => `
      <div class="pkg ${p.status} ${p.eta === today ? 'today' : ''}">
        <div class="pkg-top">
          <span class="pkg-label">${escapeHtml(p.label)}</span>
          <span class="pkg-eta">${PKG.fmtDate(p.eta)}</span>
        </div>
        <div class="pkg-meta">
          ${p.place ? `📦 ${escapeHtml(p.place)}` : ''}
          ${p.actual_place ? `<span class="arrow">→</span>${escapeHtml(p.actual_place)}` : ''}
          ${p.tracking ? ` · <span style="color:#88aabb">#${escapeHtml(p.tracking)}</span>` : ''}
          ${p.url ? ` · <a style="color:#88aabb" href="${escapeHtml(resolveTrackUrl(p.url, p.postal_code))}" target="_blank">track${p.postal_code ? ' ↗' : ''}</a>` : ''}
          ${p.postal_code && !p.url ? ` · 📮 ${escapeHtml(p.postal_code)}` : ''}
        </div>
        ${p.status === 'pending' ? `
        <div class="pkg-ctl">
          <button onclick="PKG.receive('${p.id}', '${escapeHtml(p.place||'')}')">Ontvangen</button>
          <button class="danger" onclick="PKG.del('${p.id}')">Verwijder</button>
        </div>` : `
        <div class="pkg-ctl">
          <button class="danger" onclick="PKG.del('${p.id}')">Verwijder</button>
        </div>`}
      </div>`;
    el.innerHTML = [...pending, ...received].map(card).join('');
  },
  async refresh() {
    try {
      const r = await fetch('/api/packages', {cache:'no-store'});
      PKG.render(await r.json());
    } catch (e) { console.error(e); }
  },
  async add(o) {
    if (!o.label || !o.label.trim()) return;
    await fetch('/api/packages', { method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify(Object.assign({source:'pwa'}, o))});
    PKG.refresh();
  },
  async del(id) {
    if (!confirm('Pakket verwijderen?')) return;
    await fetch('/api/packages/' + id, { method:'DELETE' });
    PKG.refresh();
  },
  async receive(id, defaultPlace) {
    const where = prompt('Waar werd het afgeleverd?', defaultPlace || '');
    if (where === null) return;
    await fetch(`/api/packages/${id}/receive`, { method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({actual_place: where})});
    PKG.refresh();
  },
};
$('pkg-add-btn').onclick = () => {
  PKG.add({
    label:       $('pkg-label').value,
    eta:         $('pkg-eta').value || new Date().toISOString().slice(0,10),
    place:       $('pkg-place').value,
    tracking:    $('pkg-tracking').value,
    postal_code: $('pkg-postal').value,
    url:         $('pkg-url').value,
  });
  ['pkg-label','pkg-place','pkg-tracking','pkg-url'].forEach(id => $(id).value = '');
  // keep postal_code sticky — usually constant for your address
};
$('pkg-eta').value = new Date().toISOString().slice(0,10);
PKG.refresh();
setInterval(PKG.refresh, 30000);
window.PKG = PKG;
