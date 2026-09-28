const $ = id => document.getElementById(id);
const names = ['A', 'X', 'L', 'B', 'S', 'T', 'PC', 'SW'];
const format = (value, width = 6) => Number(value).toString(16).toUpperCase().padStart(width, '0');
let followPc = true;
let memoryAddress = 0;
let lastRegisters = [];
let machine = null;

function showNotice(message, error = false) {
  const notice = $('notice');
  notice.textContent = message;
  notice.classList.toggle('error', error);
  notice.classList.add('show');
}

function parseAddress(text) {
  const value = text.trim();
  if (!/^(0x[0-9a-f]+|[0-9]+)$/i.test(value)) throw new Error('Use endereço decimal ou hexadecimal com prefixo 0x.');
  const number = Number(value);
  if (!Number.isInteger(number) || number < 0 || number >= 0x100000) throw new Error('Endereço fora da memória (0x00000 a 0xFFFFF).');
  return number;
}

async function request(path, method = 'GET', body = '') {
  const response = await fetch(path, { method, body: method === 'GET' ? undefined : body });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'Falha na operação.');
  return result;
}

function renderMemory(data) {
  const rows = [];
  for (let offset = 0; offset < data.memory.length; offset += 16) {
    const bytes = data.memory.slice(offset, offset + 16);
    const cells = bytes.map((byte, index) => `<td class="${data.memoryAddress + offset + index === data.registers[6] ? 'current' : ''}">${format(byte, 2)}</td>`).join('');
    const ascii = bytes.map(byte => byte >= 32 && byte <= 126 ? String.fromCharCode(byte).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])) : '·').join('');
    rows.push(`<tr><td>${format(data.memoryAddress + offset, 5)}</td>${cells}<td>${ascii}</td></tr>`);
  }
  $('memory').innerHTML = rows.join('');
}

function render(data) {
  machine = data;
  const ready = data.loaded;
  for (const id of ['step', 'run', 'reset', 'sendInput']) $(id).disabled = !ready || (id !== 'reset' && id !== 'sendInput' && data.halted);
  $('programName').textContent = ready ? data.name || 'SEM NOME' : '—';
  $('statusText').textContent = !ready ? 'Nenhum programa carregado' : data.error ? `Erro: ${data.error}` : data.halted ? 'Execução encerrada' : `${data.name || 'Programa'} em execução`;
  $('statusDot').className = 'dot' + (data.error ? ' error' : ready && !data.halted ? ' running' : '');
  $('pc').textContent = format(data.registers[6], 5);
  $('steps').textContent = data.steps.toLocaleString('pt-BR');
  $('cc').textContent = data.condition < 0 ? '<' : data.condition > 0 ? '>' : '=';
  $('inputCount').textContent = `${data.inputCount} byte(s) na fila`;
  $('output').textContent = data.output || '—';
  $('registers').innerHTML = names.map((name, index) => `<div class="register ${lastRegisters[index] !== undefined && lastRegisters[index] !== data.registers[index] ? 'highlight' : ''}"><span class="name">${name}</span><span class="hex">${format(data.registers[index], name === 'PC' ? 5 : 6)}</span><span class="decimal">${Number(data.registers[index]).toLocaleString('pt-BR')}</span></div>`).join('') + `<div class="register"><span class="name">F</span><span class="hex">${format(data.floating, 12)}</span><span class="decimal">48 bits</span></div>`;
  lastRegisters = [...data.registers];
  renderMemory(data);
  if (data.error) showNotice(data.error, true);
}

async function refresh(address = memoryAddress) {
  render(await request(`/api/state?address=${address}&count=128`));
}

async function action(path, body = '') {
  try {
    const data = await request(path, 'POST', body);
    if (followPc) {
      memoryAddress = data.registers[6] & ~15;
      $('memoryAddress').value = `0x${format(memoryAddress, 5)}`;
      await refresh(memoryAddress);
    } else await refresh(memoryAddress);
  } catch (error) { showNotice(error.message, true); }
}

$('file').addEventListener('change', async event => {
  const file = event.target.files[0];
  if (!file) return;
  $('object').value = await file.text();
  $('fileName').textContent = file.name;
});
$('example').addEventListener('click', () => {
  $('object').value = 'H^SOMA^000000^00000F\nT^000000^0F^0100051900070F20034F0000000000\nE^000000';
  $('fileName').textContent = 'Exemplo: 5 + 7 → memória 0x0000C';
  showNotice('Exemplo inserido. Clique em Carregar programa.');
});
$('load').addEventListener('click', () => {
  try { action(`/api/load?address=${parseAddress($('loadAddress').value)}`, $('object').value); }
  catch (error) { showNotice(error.message, true); }
});
$('step').addEventListener('click', () => action('/api/step'));
$('run').addEventListener('click', () => {
  try {
    const limit = Number($('limit').value);
    if (!Number.isInteger(limit) || limit < 1 || limit > 100000) throw new Error('O limite deve estar entre 1 e 100000 passos.');
    const breakpoint = $('breakpoint').value.trim();
    action(`/api/run?limit=${limit}${breakpoint ? `&breakpoint=${parseAddress(breakpoint)}` : ''}`);
  } catch (error) { showNotice(error.message, true); }
});
$('reset').addEventListener('click', () => action('/api/reset'));
$('sendInput').addEventListener('click', async () => {
  await action('/api/input', $('input').value);
  $('input').value = '';
});
$('inspect').addEventListener('click', async () => {
  try { memoryAddress = parseAddress($('memoryAddress').value) & ~15; await refresh(memoryAddress); }
  catch (error) { showNotice(error.message, true); }
});
$('follow').addEventListener('click', () => {
  followPc = !followPc;
  $('follow').textContent = `Seguir PC: ${followPc ? 'ligado' : 'desligado'}`;
  $('follow').setAttribute('aria-pressed', followPc);
  if (followPc && machine) { memoryAddress = machine.registers[6] & ~15; $('memoryAddress').value = `0x${format(memoryAddress, 5)}`; refresh(memoryAddress).catch(error => showNotice(error.message, true)); }
});
if (location.protocol === 'file:') {
  showNotice('A interface precisa do executor. No terminal, execute ./executar.sh na pasta do projeto; o navegador será aberto automaticamente.', true);
} else {
  refresh(0).catch(error => showNotice(error.message, true));
}
