// A página é pequena e usa os IDs do HTML como ligação direta aos controles.
const $ = id => document.getElementById(id);
const names = ['A', 'X', 'L', 'B', 'S', 'T', 'PC', 'SW'];
const format = (value, width = 6) => Number(value).toString(16).toUpperCase().padStart(width, '0');
let followPc = true;
let memoryAddress = 0;
let lastRegisters = [];
let machine = null;
// PC pode ultrapassar o último byte após executar uma instrução no fim da memória.
const pcAddress = data => Math.min(data.registers[6], 0xFFFFF) & ~15;

function showNotice(message, error = false) {
  const notice = $('notice');
  notice.textContent = message;
  notice.classList.toggle('error', error);
  notice.classList.add('show');
}

function parseAddress(text) {
  // Na interface, hexadecimal exige 0x; sem prefixo, o número é decimal.
  const value = text.trim();
  if (!/^(0x[0-9a-f]+|[0-9]+)$/i.test(value)) throw new Error('Use endereço decimal ou hexadecimal com prefixo 0x.');
  const number = Number(value);
  if (!Number.isInteger(number) || number < 0 || number >= 0x100000) throw new Error('Endereço fora da memória (0x00000 a 0xFFFFF).');
  return number;
}

async function request(path, method = 'GET', body = '') {
  // Respostas HTTP com erro também trazem JSON com uma mensagem para o usuário.
  const response = await fetch(path, { method, body: method === 'GET' ? undefined : body });
  const result = await response.json();
  if (!response.ok) throw new Error(result.error || 'Falha na operação.');
  return result;
}

function renderMemory(data) {
  // A API envia uma janela curta; cada linha exibe 16 bytes e sua visão ASCII.
  const rows = [];
  for (let offset = 0; offset < data.memory.length; offset += 16) {
    const bytes = data.memory.slice(offset, offset + 16);
    const cells = bytes.map((byte, index) => `<td class="${data.memoryAddress + offset + index === data.registers[6] ? 'current' : ''}">${format(byte, 2)}</td>`).join('');
    // O conteúdo do programa não deve ser interpretado como HTML na coluna ASCII.
    const ascii = bytes.map(byte => byte >= 32 && byte <= 126 ? String.fromCharCode(byte).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])) : '·').join('');
    rows.push(`<tr><td>${format(data.memoryAddress + offset, 5)}</td>${cells}<td>${ascii}</td></tr>`);
  }
  $('memory').innerHTML = rows.join('');
}

function render(data) {
  // Um único estado retornado pelo executor atualiza todos os painéis.
  machine = data;
  const ready = data.loaded;
  for (const id of ['step', 'run', 'reset']) $(id).disabled = !ready || (id !== 'reset' && data.halted);
  $('programName').textContent = ready ? data.name || 'SEM NOME' : '—';
  $('statusText').textContent = !ready ? 'Nenhum programa carregado' : data.error ? `Erro: ${data.error}` : data.halted ? 'Execução encerrada' : `${data.name || 'Programa'} pronto para continuar`;
  $('statusDot').className = 'dot' + (data.error ? ' error' : ready && !data.halted ? ' running' : '');
  $('pc').textContent = format(data.registers[6], 5);
  $('steps').textContent = data.steps.toLocaleString('pt-BR');
  $('cc').textContent = data.condition < 0 ? '<' : data.condition > 0 ? '>' : '=';
  // Compara com a resposta anterior para destacar apenas registradores alterados.
  $('registers').innerHTML = names.map((name, index) => `<div class="register ${lastRegisters[index] !== undefined && lastRegisters[index] !== data.registers[index] ? 'highlight' : ''}"><span class="name">${name}</span><span class="hex">${format(data.registers[index], name === 'PC' ? 5 : 6)}</span><span class="decimal">${Number(data.registers[index]).toLocaleString('pt-BR')}</span></div>`).join('') + `<div class="register"><span class="name">F</span><span class="hex">${format(data.floating, 12)}</span><span class="decimal">48 bits</span></div>`;
  lastRegisters = [...data.registers];
  renderMemory(data);
  if (data.error) showNotice(data.error, true);
  else $('notice').classList.remove('show');
}

async function refresh(address = memoryAddress) {
  // Busca estado e a janela de memória escolhida, sem executar instruções.
  render(await request(`/api/state?address=${address}&count=128`));
}

async function action(path, body = '') {
  // Após executar um comando, atualiza o estado e opcionalmente acompanha o PC.
  try {
    const data = await request(path, 'POST', body);
    render(data);
    if (followPc) {
      memoryAddress = pcAddress(data);
      $('memoryAddress').value = `0x${format(memoryAddress, 5)}`;
      await refresh(memoryAddress);
    } else await refresh(memoryAddress);
  } catch (error) { showNotice(error.message, true); }
}

$('file').addEventListener('change', async event => {
  // A escolha do arquivo apenas preenche a área de texto; carregar é ação separada.
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
$('inspect').addEventListener('click', async () => {
  // Alinha o endereço à primeira coluna de uma linha de 16 bytes.
  try { memoryAddress = parseAddress($('memoryAddress').value) & ~15; await refresh(memoryAddress); }
  catch (error) { showNotice(error.message, true); }
});
$('follow').addEventListener('click', () => {
  // Quando desligado, a janela de memória fica no endereço escolhido manualmente.
  followPc = !followPc;
  $('follow').textContent = `Seguir PC: ${followPc ? 'ligado' : 'desligado'}`;
  $('follow').setAttribute('aria-pressed', followPc);
  if (followPc && machine) { memoryAddress = pcAddress(machine); $('memoryAddress').value = `0x${format(memoryAddress, 5)}`; refresh(memoryAddress).catch(error => showNotice(error.message, true)); }
});
if (location.protocol === 'file:') {
  // Abrir o HTML isolado não disponibiliza a API do executor.
  showNotice('A interface precisa do executor. No terminal, execute ./executar.sh na pasta do projeto; o navegador será aberto automaticamente.', true);
} else {
  refresh(0).catch(error => showNotice(error.message, true));
}
