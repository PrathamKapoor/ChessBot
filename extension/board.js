const glyph = {P:'♙',N:'♘',B:'♗',R:'♖',Q:'♕',K:'♔',p:'♟',n:'♞',b:'♝',r:'♜',q:'♛',k:'♚'};
const el = id => document.getElementById(id);
let moves = [], info = null, selected = null, busy = false, lastEngine = null, redoStack = [], lastOpening = null;
let marks = new Set(), arrows = [], markStart = null, audioContext = null;
const files = 'abcdefgh';
function sound(kind='move') {
  if (!el('sound').checked) return;
  try {
    audioContext ||= new AudioContext();
    const now=audioContext.currentTime;
    const tones=kind==='mate' ? [523,659,784] : kind==='check' ? [440,580] : [kind==='capture'?250:480];
    tones.forEach((frequency,i)=>{const oscillator=audioContext.createOscillator(), gain=audioContext.createGain(); oscillator.type='sine'; oscillator.frequency.value=frequency; gain.gain.setValueAtTime(0.0001,now+i*.12); gain.gain.exponentialRampToValueAtTime(.12,now+i*.12+.015); gain.gain.exponentialRampToValueAtTime(.0001,now+i*.12+.13); oscillator.connect(gain).connect(audioContext.destination); oscillator.start(now+i*.12); oscillator.stop(now+i*.12+.14);});
  } catch (_) { /* Audio may be disabled by browser settings. */ }
}
function drawArrows(board, columns, ranks) {
  const ns='http://www.w3.org/2000/svg', svg=document.createElementNS(ns,'svg');
  svg.setAttribute('viewBox','0 0 8 8'); svg.classList.add('arrows'); svg.setAttribute('aria-hidden','true');
  const defs=document.createElementNS(ns,'defs'), marker=document.createElementNS(ns,'marker');
  marker.setAttribute('id','arrowhead'); marker.setAttribute('markerWidth','4'); marker.setAttribute('markerHeight','4'); marker.setAttribute('refX','2.6'); marker.setAttribute('refY','2'); marker.setAttribute('orient','auto');
  const head=document.createElementNS(ns,'path'); head.setAttribute('d','M 0 0 L 4 2 L 0 4 z'); head.setAttribute('fill','#f38f21'); marker.append(head); defs.append(marker); svg.append(defs);
  for(const [from,to] of arrows){const line=document.createElementNS(ns,'line'); line.setAttribute('x1',columns.indexOf(from[0])+.5); line.setAttribute('y1',ranks.indexOf(Number(from[1]))+.5); line.setAttribute('x2',columns.indexOf(to[0])+.5); line.setAttribute('y2',ranks.indexOf(Number(to[1]))+.5); line.setAttribute('stroke','#f38f21'); line.setAttribute('stroke-width','.18'); line.setAttribute('stroke-linecap','round'); line.setAttribute('marker-end','url(#arrowhead)'); svg.append(line);}
  board.append(svg);
}

async function save() {
  await chrome.storage.local.set({game:{moves, side:el('side').value, time:el('time').value, engine:el('engine').value, book:el('book').checked, lastEngine, lastOpening, redoStack}});
}
async function call(action) {
  const response = await fetch('http://127.0.0.1:8765/', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({action, moves, engine:el('engine').value, book:el('book').checked, movetime:Number(el('time').value)})});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || 'Bridge error');
  return data;
}
function render() {
  if (!info) return;
  el('engine').disabled = busy;
  el('redo').disabled = busy || !redoStack.length;
  el('undo').disabled = busy || !moves.length;
  const side = el('side').value;
  el('top-label').textContent = side === 'white' ? 'BLACK' : 'WHITE';
  el('bottom-label').textContent = side.toUpperCase();
  el('engine-label').textContent = el('engine').value === 'stockfish' ? 'STOCKFISH' : 'POCKETCHESS';
  const ranks = side === 'white' ? [8,7,6,5,4,3,2,1] : [1,2,3,4,5,6,7,8];
  const columns = side === 'white' ? [...files] : [...files].reverse();
  const board = el('board'); board.replaceChildren();
  for (const rank of ranks) for (const file of columns) {
    const square = file + rank, piece = info.pieces[square];
    const button = document.createElement('button');
    button.className = `square ${(files.indexOf(file)+rank)%2===0 ? 'light':'dark'}`;
    if (selected === square) button.classList.add('selected');
    if (marks.has(square)) button.classList.add('marked');
    if (info.checked_king === square) button.classList.add('checked');
    if (selected && info.legal.some(move=>move.slice(0,4)===selected+square)) button.classList.add(piece ? 'legal-capture' : 'legal-target');
    if (moves.length && (moves.at(-1).slice(0,2) === square || moves.at(-1).slice(2,4) === square)) button.classList.add('last');
    button.setAttribute('aria-label', `${square}${piece ? ' ' + piece : ' empty'}`);
    if (piece) { const span=document.createElement('span'); span.className=`piece ${piece===piece.toUpperCase()?'white':'black'}`; span.textContent=glyph[piece]; button.append(span); }
    if (rank === ranks.at(-1) || file === columns[0]) { const coord=document.createElement('span'); coord.className='coord'; coord.textContent=(file===columns[0]?rank:'')+(rank===ranks.at(-1)?file:''); button.append(coord); }
    button.addEventListener('click', () => clickSquare(square));
    button.addEventListener('pointerdown', event => {if(event.button===2){event.preventDefault();markStart=square;}});
    button.addEventListener('pointerup', event => {if(event.button!==2||!markStart)return; event.preventDefault(); if(markStart===square){marks.has(square)?marks.delete(square):marks.add(square);} else {const index=arrows.findIndex(([a,b])=>a===markStart&&b===square); if(index>=0)arrows.splice(index,1); else arrows.push([markStart,square]);} markStart=null;render();});
    button.addEventListener('contextmenu', event=>event.preventDefault()); board.append(button);
  }
  drawArrows(board,columns,ranks);
  el('lastmove').textContent = lastEngine ? `${lastEngine.slice(0,2)} → ${lastEngine.slice(2,4)}` : '—';
  el('lastsan').textContent = lastOpening ? `${lastOpening} · Copy to Chess.com` : (lastEngine ? 'Copy this move to Chess.com' : 'Engine move');
  if (info.outcome) { el('status').textContent = info.termination === 'checkmate' ? `Checkmate! ${info.winner === side ? `${el('engine').value === 'stockfish' ? 'Stockfish' : 'PocketChess'} wins` : 'Bot wins'} · ${info.outcome}` : `Draw · ${info.outcome}`; el('instruction').textContent=lastEngine ? 'Copy the final move to Chess.com.' : 'Game finished. Start a new game to play again.'; }
  else if (busy) { el('status').textContent='Engine is thinking…'; el('instruction').textContent='One moment.'; }
  else if (info.turn === side) { el('status').textContent='Engine ready'; el('instruction').textContent='Press Reconnect / retry if no move appears.'; }
  else { el('status').textContent=info.checked_king ? 'Check! Enter the bot’s move' : "Enter the bot’s move"; el('instruction').textContent='Click the bot’s piece, then its destination square.'; }
}
async function update(think = true) {
  try {
    busy=true; render(); info=await call('position'); el('connection').textContent='Bridge connected';
    render();
    if (think && !info.outcome && info.turn===el('side').value) {
      const result=await call('think');
      if (!result.engine_name) throw new Error('The bridge is out of date; stop it and start python tools/companion_bridge.py again');
      if (el('engine').value === 'stockfish' && !result.engine_name.toLowerCase().includes('stockfish')) throw new Error('The requested Stockfish engine was not started');
      if (!result.bestmove || !info.legal.includes(result.bestmove)) throw new Error('Engine returned an invalid move');
      const capture=Boolean(info.pieces[result.bestmove.slice(2,4)]); redoStack=[]; moves.push(result.bestmove); lastEngine=result.bestmove; lastOpening=result.opening||null; info=await call('position'); sound(info.termination==='checkmate'?'mate':info.checked_king?'check':capture?'capture':'move');
    }
    await save();
  } catch(error) {
    el('connection').textContent='Bridge unavailable';
    el('status').textContent='Could not reach the engine';
    el('instruction').textContent=`${error.message}. Start the bridge and click Reconnect / retry.`;
    busy=false; el('engine').disabled=false; return;
  }
  busy=false; render();
}
async function clickSquare(square) {
  if (busy || !info || info.outcome || info.turn===el('side').value) return;
  const piece=info.pieces[square];
  if (!selected) { if (piece && (piece===piece.toUpperCase()) === (info.turn==='white')) { selected=square; render(); } return; }
  if (square===selected) { selected=null; render(); return; }
  const prefix=selected+square;
  let candidates=info.legal.filter(move=>move.startsWith(prefix));
  if (!candidates.length) {
    selected=piece && (piece===piece.toUpperCase()) === (info.turn==='white') ? square : null;
    el('instruction').textContent='That move is not legal. Choose a highlighted piece and destination.';
    render(); return;
  }
  let move=candidates[0];
  if (candidates.length>1) {
    const choice=prompt('Promote to: q (queen), r (rook), b (bishop), or n (knight)', 'q');
    if (choice===null) { selected=null; render(); return; }
    move=candidates.find(m=>m[4]===choice.toLowerCase());
    if (!move) { selected=null; render(); el('instruction').textContent='Promotion cancelled: choose q, r, b, or n.'; return; }
  }
  const capture=Boolean(info.pieces[square]); redoStack=[]; moves.push(move); selected=null; marks.clear(); arrows=[]; sound(capture?'capture':'move'); await save(); await update();
}
el('newgame').addEventListener('click', async()=>{ if(busy)return; moves=[];redoStack=[];selected=null;lastEngine=null;lastOpening=null;marks.clear();arrows=[];await save();await update(); });
el('undo').addEventListener('click', async()=>{ if(busy||!moves.length)return; redoStack.push({moves:[...moves],lastEngine,lastOpening}); const count=info && info.turn===el('side').value ? 1 : Math.min(2,moves.length); moves.splice(-count);lastEngine=null;lastOpening=null;selected=null;await save();await update(false); });
el('redo').addEventListener('click', async()=>{ if(busy||!redoStack.length)return; const snapshot=redoStack.pop(); moves=snapshot.moves; lastEngine=snapshot.lastEngine; lastOpening=snapshot.lastOpening||null; selected=null; await save(); await update(false); });
el('retry').addEventListener('click',()=>{if(!busy)update();});
el('time').addEventListener('change',save);
el('book').addEventListener('change',save);
el('engine').addEventListener('change',async()=>{if(busy)return; await save(); render();});
el('clear-marks').addEventListener('click',()=>{marks.clear();arrows=[];render();});
el('side').addEventListener('change',async()=>{moves=[];redoStack=[];selected=null;lastEngine=null;lastOpening=null;marks.clear();arrows=[];await save();await update();});
(async()=>{ const stored=await chrome.storage.local.get('game'); if(stored.game){moves=stored.game.moves||[];el('side').value=stored.game.side||'white';el('time').value=stored.game.time||'1000';el('engine').value=stored.game.engine||'stockfish';if(stored.game.book!==undefined)el('book').checked=stored.game.book;lastEngine=stored.game.lastEngine||null;lastOpening=stored.game.lastOpening||null;redoStack=stored.game.redoStack||[];} await update(!redoStack.length); })();
