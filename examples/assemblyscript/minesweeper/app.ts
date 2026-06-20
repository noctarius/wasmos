import { Button, Context, POINTER_LEFT, POINTER_RIGHT, Rect, Surface } from "./libui";

const BOARD_W: i32 = 8;
const BOARD_H: i32 = 8;
const BOARD_CELLS: i32 = BOARD_W * BOARD_H;
const MINE_COUNT: i32 = 10;

const WINDOW_W: i32 = 232;
const WINDOW_H: i32 = 264;
const TOP_BAR_H: i32 = 32;
const PADDING: i32 = 16;
const CELL: i32 = 24;
const RESET_SIZE: i32 = 20;

const COLOR_BG: u32 = 0xFF92A1B4;
const COLOR_PANEL: u32 = 0xFFC3CEDA;
const COLOR_HIDDEN: u32 = 0xFFBCC7D3;
const COLOR_REVEALED: u32 = 0xFFE7EDF4;
const COLOR_GRID_DARK: u32 = 0xFF718295;
const COLOR_GRID_LIGHT: u32 = 0xFFF7FBFF;
const COLOR_MINE: u32 = 0xFF232A34;
const COLOR_MINE_HIT: u32 = 0xFFCC5448;
const COLOR_FLAG: u32 = 0xFFD64C4C;
const COLOR_RESET: u32 = 0xFFF2C14E;
const COLOR_RESET_BORDER: u32 = 0xFF9C6B00;

let g_rng: u32 = 0x00C0FFEE;
let g_firstMove: bool = true;
let g_lost: bool = false;
let g_won: bool = false;
let g_revealedCount: i32 = 0;

let g_mines = new Uint8Array(BOARD_CELLS);
let g_revealed = new Uint8Array(BOARD_CELLS);
let g_flagged = new Uint8Array(BOARD_CELLS);
let g_adjacent = new Uint8Array(BOARD_CELLS);

let g_resetButton = new Button();
let g_boardRect = new Rect(PADDING, TOP_BAR_H + PADDING, BOARD_W * CELL, BOARD_H * CELL);

function boardX(): i32 { return PADDING; }
function boardY(): i32 { return TOP_BAR_H + PADDING; }
function cellIndex(x: i32, y: i32): i32 { return y * BOARD_W + x; }

function randNext(): u32 {
  g_rng = g_rng * 1664525 + 1013904223;
  return g_rng;
}

function resetState(): void {
  g_firstMove = true;
  g_lost = false;
  g_won = false;
  g_revealedCount = 0;
  g_rng = 0x00C0FFEE;
  for (let i = 0; i < BOARD_CELLS; i++) {
    g_mines[i] = 0;
    g_revealed[i] = 0;
    g_flagged[i] = 0;
    g_adjacent[i] = 0;
  }
}

function placeMines(safeIndex: i32): void {
  let placed = 0;
  while (placed < MINE_COUNT) {
    const candidate = <i32>(randNext() % BOARD_CELLS);
    if (candidate == safeIndex || g_mines[candidate] != 0) {
      continue;
    }
    g_mines[candidate] = 1;
    placed++;
  }
  for (let y = 0; y < BOARD_H; y++) {
    for (let x = 0; x < BOARD_W; x++) {
      const idx = cellIndex(x, y);
      if (g_mines[idx] != 0) continue;
      let count: u8 = 0;
      for (let yy = y - 1; yy <= y + 1; yy++) {
        for (let xx = x - 1; xx <= x + 1; xx++) {
          if (xx < 0 || yy < 0 || xx >= BOARD_W || yy >= BOARD_H) continue;
          if (xx == x && yy == y) continue;
          if (g_mines[cellIndex(xx, yy)] != 0) count++;
        }
      }
      g_adjacent[idx] = count;
    }
  }
}

function revealFrom(index: i32): void {
  if (index < 0 || index >= BOARD_CELLS) return;
  if (g_revealed[index] != 0 || g_flagged[index] != 0) return;

  if (g_firstMove) {
    placeMines(index);
    g_firstMove = false;
  }

  if (g_mines[index] != 0) {
    g_lost = true;
    for (let i = 0; i < BOARD_CELLS; i++) {
      if (g_mines[i] != 0) g_revealed[i] = 1;
    }
    return;
  }

  const queue = new Int32Array(BOARD_CELLS);
  let head = 0;
  let tail = 0;
  queue[tail++] = index;
  while (head < tail) {
    const current = queue[head++];
    if (g_revealed[current] != 0 || g_flagged[current] != 0) continue;
    g_revealed[current] = 1;
    g_revealedCount++;
    if (g_adjacent[current] != 0) continue;
    const x = current % BOARD_W;
    const y = current / BOARD_W;
    for (let yy = y - 1; yy <= y + 1; yy++) {
      for (let xx = x - 1; xx <= x + 1; xx++) {
        if (xx < 0 || yy < 0 || xx >= BOARD_W || yy >= BOARD_H) continue;
        const next = cellIndex(xx, yy);
        if (g_revealed[next] == 0 && g_mines[next] == 0) {
          queue[tail++] = next;
        }
      }
    }
  }

  if (g_revealedCount >= BOARD_CELLS - MINE_COUNT) {
    g_won = true;
  }
}

function toggleFlag(index: i32): void {
  if (index < 0 || index >= BOARD_CELLS) return;
  if (g_revealed[index] != 0 || g_won || g_lost) return;
  g_flagged[index] = g_flagged[index] == 0 ? 1 : 0;
}

function flaggedCount(): i32 {
  let total = 0;
  for (let i = 0; i < BOARD_CELLS; i++) total += g_flagged[i];
  return total;
}

function digitColor(digit: i32): u32 {
  if (digit == 1) return 0xFF315BDE;
  if (digit == 2) return 0xFF1F8B4C;
  if (digit == 3) return 0xFFC5423B;
  if (digit == 4) return 0xFF5A2BAF;
  if (digit == 5) return 0xFF8C2D22;
  if (digit == 6) return 0xFF1F7A7A;
  if (digit == 7) return 0xFF2B3342;
  return 0xFF5C6470;
}

function updateTitle(ctx: Context): void {
  let text = "Minesweeper";
  if (g_lost) {
    text = "Minesweeper - boom";
  } else if (g_won) {
    text = "Minesweeper - cleared";
  } else {
    text = "Minesweeper - " + flaggedCount().toString() + "/" + MINE_COUNT.toString() + " flags";
  }
  let _ = ctx.setTitle(text);
}

function drawBevel(surface: Surface, x: i32, y: i32, w: i32, h: i32, pressed: bool): void {
  surface.fillRect(x, y, w, h, pressed ? COLOR_REVEALED : COLOR_HIDDEN);
  surface.strokeRect(x, y, w, h, 1, COLOR_GRID_DARK);
  if (!pressed) {
    surface.fillRect(x + 1, y + 1, w - 2, 1, COLOR_GRID_LIGHT);
    surface.fillRect(x + 1, y + 1, 1, h - 2, COLOR_GRID_LIGHT);
  }
}

function drawResetButton(surface: Surface, x: i32, y: i32): void {
  drawBevel(surface, x, y, RESET_SIZE, RESET_SIZE, false);
  surface.fillCircle(x + 10, y + 10, 7, COLOR_RESET);
  surface.fillCircle(x + 7, y + 8, 1, COLOR_RESET_BORDER);
  surface.fillCircle(x + 13, y + 8, 1, COLOR_RESET_BORDER);
  surface.fillRect(x + 6, y + 13, 8, 1, COLOR_RESET_BORDER);
}

function drawFlag(surface: Surface, x: i32, y: i32): void {
  surface.fillRect(x + 11, y + 6, 2, 12, COLOR_GRID_DARK);
  surface.fillRect(x + 13, y + 7, 7, 5, COLOR_FLAG);
  surface.fillRect(x + 8, y + 18, 10, 2, COLOR_GRID_DARK);
}

function drawMine(surface: Surface, x: i32, y: i32, hit: bool): void {
  if (hit) {
    surface.fillRect(x + 3, y + 3, CELL - 6, CELL - 6, COLOR_MINE_HIT);
  }
  surface.fillCircle(x + 12, y + 12, 5, COLOR_MINE);
  surface.fillRect(x + 11, y + 4, 2, 16, COLOR_MINE);
  surface.fillRect(x + 4, y + 11, 16, 2, COLOR_MINE);
}

function drawBoard(surface: Surface): void {
  for (let y = 0; y < BOARD_H; y++) {
    for (let x = 0; x < BOARD_W; x++) {
      const idx = cellIndex(x, y);
      const px = boardX() + x * CELL;
      const py = boardY() + y * CELL;
      const revealed = g_revealed[idx] != 0;
      if (!revealed) {
        drawBevel(surface, px, py, CELL, CELL, false);
        if (g_flagged[idx] != 0) {
          drawFlag(surface, px, py);
        }
        continue;
      }

      surface.fillRect(px, py, CELL, CELL, COLOR_REVEALED);
      surface.strokeRect(px, py, CELL, CELL, 1, COLOR_GRID_DARK);
      if (g_mines[idx] != 0) {
        drawMine(surface, px, py, g_lost);
      } else if (g_adjacent[idx] != 0) {
        surface.drawDigit3x5(px + 7, py + 5, g_adjacent[idx], 3, digitColor(g_adjacent[idx]));
      }
    }
  }
}

function render(ctx: Context): void {
  const surface = ctx.beginFrame();
  surface.clear(COLOR_BG);
  surface.fillRect(8, 8, ctx.contentWidth() - 16, ctx.contentHeight() - 16, COLOR_PANEL);
  surface.strokeRect(8, 8, ctx.contentWidth() - 16, ctx.contentHeight() - 16, 1, COLOR_GRID_DARK);
  surface.fillRect(16, 16, ctx.contentWidth() - 32, TOP_BAR_H, COLOR_REVEALED);
  surface.strokeRect(16, 16, ctx.contentWidth() - 32, TOP_BAR_H, 1, COLOR_GRID_DARK);
  const resetX = ctx.contentWidth() - 16 - RESET_SIZE - 8;
  const resetY = 22;
  g_resetButton.setBounds(resetX, resetY, RESET_SIZE, RESET_SIZE);
  drawResetButton(surface, resetX, resetY);
  drawBoard(surface);
}

function handleInput(ctx: Context): void {
  if (ctx.activate(g_resetButton, POINTER_LEFT)) {
    resetState();
    updateTitle(ctx);
    return;
  }

  if (!ctx.hitTest(g_boardRect)) {
    return;
  }

  const gx = (ctx.pointerX() - boardX()) / CELL;
  const gy = (ctx.pointerY() - boardY()) / CELL;
  const idx = cellIndex(gx, gy);
  if (ctx.pointerPressed(POINTER_LEFT) && !g_won && !g_lost) {
    revealFrom(idx);
    updateTitle(ctx);
  } else if (ctx.pointerPressed(POINTER_RIGHT)) {
    toggleFlag(idx);
    updateTitle(ctx);
  }
}

export function main(_args: Array<string>): i32 {
  const ctx = Context.open(WINDOW_W, WINDOW_H, "Minesweeper");
  if (ctx == null) {
    return 1;
  }
  resetState();
  while (!ctx.shouldClose()) {
    ctx.pump();
    handleInput(ctx);
    render(ctx);
    if (!ctx.endFrame()) {
      ctx.destroy();
      return 2;
    }
    ctx.yield();
  }
  ctx.destroy();
  return 0;
}
