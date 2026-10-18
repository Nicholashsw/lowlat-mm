(* Two quoting rules.

   Baseline: strategy/MarketMaker.cpp, line for line. Quote around an
   inventory-skewed mid, stay inside the visible book, requote when the target
   moves by at least requote_thresh, and stop quoting the side that would breach
   max_position.

   Micro (results/quoting/PREREG.md): fair value is the microprice; quotes join the
   touch; a resting quote is moved only when fair value has moved a tick since the
   quotes were placed, while a missing side is placed straight away; both quotes
   step k ticks away from the side that adds exposure per 10 contracts held, without
   crossing the book, and the side that would go past limit contracts is not quoted;
   the offer is pulled when book imbalance is above theta, the bid below -theta. *)

open Types

type quoting = Baseline | Micro of { k : int; limit : int; theta : float }

type config = {
  quoting : quoting;
  half_spread : int;
  min_spread : int;
  quote_size : int;
  max_position : int;
  inv_skew : float;
  requote_thresh : int;
}

type live = { mutable bid_id : int; mutable bid_px : int; mutable ask_id : int; mutable ask_px : int }

type t = {
  cfg : config;
  oms : Oms.t;
  risk : Risk.t;
  send : int -> Event.t -> unit;
  live : live;
  mutable fair_at_quote : float;  (* microprice when the resting quotes were placed (Micro) *)
}

let create cfg oms risk ~send =
  { cfg; oms; risk; send; live = { bid_id = 0; bid_px = 0; ask_id = 0; ask_px = 0 }; fair_at_quote = nan }

let cancel t ts id = if Oms.cancel t.oms id then t.send ts (Event.Cancel_order { id })

let drop_bid t ts =
  if t.live.bid_id <> 0 then begin
    cancel t ts t.live.bid_id;
    t.live.bid_id <- 0
  end

let drop_ask t ts =
  if t.live.ask_id <> 0 then begin
    cancel t ts t.live.ask_id;
    t.live.ask_id <- 0
  end

let cancel_all t ts =
  drop_bid t ts;
  drop_ask t ts

let try_submit t ts side px =
  match Risk.check t.risk t.oms side t.cfg.quote_size with
  | Error _ -> ()
  | Ok () -> (
      let o = Oms.submit t.oms ~side ~px ~qty:t.cfg.quote_size in
      t.send ts (Event.New_order { id = o.id; side; px; qty = o.qty });
      match side with
      | Buy ->
          t.live.bid_id <- o.id;
          t.live.bid_px <- px
      | Sell ->
          t.live.ask_id <- o.id;
          t.live.ask_px <- px)

let baseline_quote t ts (q : Feed.quote) =
  let c = t.cfg in
  if t.risk.killed then cancel_all t ts
  else if q.bid_sz <= 0 || q.ask_sz <= 0 || q.bid_px >= q.ask_px then cancel_all t ts
  else if q.ask_px - q.bid_px < c.min_spread then cancel_all t ts
  else begin
    let mid = (q.bid_px + q.ask_px) / 2 in
    let net = t.oms.pos.net in
    let skewed = float_of_int mid -. (c.inv_skew *. float_of_int net) in
    let target_bid = min (int_of_float (Float.floor (skewed -. float_of_int c.half_spread))) (q.ask_px - 1) in
    let target_ask = max (int_of_float (Float.ceil (skewed +. float_of_int c.half_spread))) (q.bid_px + 1) in
    if target_bid >= target_ask then cancel_all t ts
    else begin
      if net + c.quote_size <= c.max_position then begin
        if t.live.bid_id = 0 || abs (t.live.bid_px - target_bid) >= c.requote_thresh then begin
          drop_bid t ts;
          try_submit t ts Buy target_bid
        end
      end
      else drop_bid t ts;
      if net - c.quote_size >= -c.max_position then begin
        if t.live.ask_id = 0 || abs (t.live.ask_px - target_ask) >= c.requote_thresh then begin
          drop_ask t ts;
          try_submit t ts Sell target_ask
        end
      end
      else drop_ask t ts
    end
  end

(* A quote is resting if its order exists and is not finished. *)
let resting t id = id <> 0 && match Oms.find t.oms id with Some o -> not (Oms.terminal o) | None -> false

let micro_quote t ts (q : Feed.quote) ~k ~limit ~theta =
  if t.risk.killed || q.bid_sz <= 0 || q.ask_sz <= 0 || q.bid_px >= q.ask_px then cancel_all t ts
  else begin
    let bs = float_of_int q.bid_sz and az = float_of_int q.ask_sz in
    let fair = ((float_of_int q.bid_px *. az) +. (float_of_int q.ask_px *. bs)) /. (bs +. az) in
    let imbalance = (bs -. az) /. (bs +. az) in
    let net = t.oms.pos.net in
    let shift = k * net / 10 in                           (* ticks, positive when long *)
    let target_bid = min (q.bid_px - shift) (q.ask_px - 1) in
    let target_ask = max (q.ask_px - shift) (q.bid_px + 1) in
    let moved = Float.is_nan t.fair_at_quote || Float.abs (fair -. t.fair_at_quote) >= 1.0 in
    let want_bid = net + t.cfg.quote_size <= limit && imbalance >= -.theta in
    let want_ask = net - t.cfg.quote_size >= -limit && imbalance <= theta in
    let placed = ref false in
    if not want_bid then drop_bid t ts
    else if not (resting t t.live.bid_id) then begin
      t.live.bid_id <- 0;
      try_submit t ts Buy target_bid;
      placed := true
    end
    else if moved && t.live.bid_px <> target_bid then begin
      drop_bid t ts;
      try_submit t ts Buy target_bid;
      placed := true
    end;
    if not want_ask then drop_ask t ts
    else if not (resting t t.live.ask_id) then begin
      t.live.ask_id <- 0;
      try_submit t ts Sell target_ask;
      placed := true
    end
    else if moved && t.live.ask_px <> target_ask then begin
      drop_ask t ts;
      try_submit t ts Sell target_ask;
      placed := true
    end;
    if !placed || moved then t.fair_at_quote <- fair
  end

let on_quote t ts q =
  match t.cfg.quoting with
  | Baseline -> baseline_quote t ts q
  | Micro { k; limit; theta } -> micro_quote t ts q ~k ~limit ~theta
