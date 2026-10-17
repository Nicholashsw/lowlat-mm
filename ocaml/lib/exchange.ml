(* The simulated exchange: acks, marketable fills, and passive fills under one of
   four rules.

   Touch and Through are the C++ simulator's two rules. A resting buy fills when
   the best ask reaches its price (Touch) or goes below it (Through), as if the
   order were first in the queue. Touch is what produced the README's ES result.

   The two Queue rules track where the order would stand in the queue, using the
   exchange's trade prints as well as the top of book:

   - An order joining the best price queues behind the size displayed there. An
     order priced inside the spread is alone at its price. An order behind the
     best has an unknown position until its price becomes the best again, when it
     is placed behind everything displayed.
   - A trade print at the order's price consumes the queue ahead of it first; the
     order fills only from the part of the print that reaches it, so fills can be
     partial.
   - A print or an offer strictly through the order's price fills it outright, and
     an offer at its price fills it once nothing is ahead.
   - When the displayed size at the order's price shrinks by more than the trades
     printed there, the difference was cancelled. Queue_behind assumes every
     cancel came from behind the order (the pessimistic bound). Queue_proportional
     spreads cancels evenly over the queue, so the share ahead of the order is
     ahead / queue.
   - If the order's price drops out of the book on its side (the best bid falls
     below a resting bid), everything that was ahead has gone and the order is
     first. *)

open Types

type model = Touch | Through | Queue_behind | Queue_proportional

let model_of_string = function
  | "touch" -> Touch
  | "through" -> Through
  | "queue-behind" -> Queue_behind
  | "queue-proportional" -> Queue_proportional
  | s -> invalid_arg ("unknown fill model: " ^ s)

let string_of_model = function
  | Touch -> "touch"
  | Through -> "through"
  | Queue_behind -> "queue-behind"
  | Queue_proportional -> "queue-proportional"

type config = { ack_latency : int; fill_latency : int; model : model }

type resting = {
  id : int;
  side : side;
  px : int;
  mutable leaves : int;
  mutable ahead : int option;  (* None: behind the best, position unknown *)
  mutable level_size : int option;  (* displayed size at our price at the last quote *)
  mutable traded : int;  (* printed at our price since that quote *)
}

module IntMap = Map.Make (Int)

type t = {
  cfg : config;
  emit : int -> Event.t -> unit;
  mutable bid : (int * int) option;
  mutable ask : (int * int) option;
  mutable resting : resting IntMap.t;  (* ordered by id, as the C++ std::map *)
}

let create cfg ~emit = { cfg; emit; bid = None; ask = None; resting = IntMap.empty }

let resting_count t = IntMap.cardinal t.resting

let queue_ahead t id = Option.bind (IntMap.find_opt id t.resting) (fun r -> r.ahead)

(* x is a better price than y for an order on [side]. *)
let better side x y = match side with Buy -> x > y | Sell -> x < y

let same_best t = function Buy -> t.bid | Sell -> t.ask

let opposite_best t = function Buy -> t.ask | Sell -> t.bid

let fill t r ts qty =
  let qty = min qty r.leaves in
  if qty > 0 then begin
    r.leaves <- r.leaves - qty;
    let final = r.leaves = 0 in
    if final then t.resting <- IntMap.remove r.id t.resting;
    t.emit (ts + t.cfg.fill_latency) (Event.Fill { id = r.id; px = r.px; qty; final })
  end

let is_queue_model t = match t.cfg.model with Queue_behind | Queue_proportional -> true | _ -> false

let on_new_order t ts ~id ~side ~px ~qty =
  t.emit (ts + t.cfg.ack_latency) (Event.Ack { id });
  let marketable =
    match opposite_best t side with Some (opx, _) when not (better side opx px) -> Some opx | _ -> None
  in
  match marketable with
  | Some opx -> t.emit (ts + t.cfg.fill_latency) (Event.Fill { id; px = opx; qty; final = true })
  | None ->
      let ahead, level_size =
        match same_best t side with
        | Some (bpx, bsz) when bpx = px -> (Some bsz, Some bsz)
        | Some (bpx, _) when better side bpx px -> (None, None)
        | _ -> (Some 0, None)
      in
      t.resting <- IntMap.add id { id; side; px; leaves = qty; ahead; level_size; traded = 0 } t.resting

let on_cancel t ts ~id =
  let accepted = IntMap.mem id t.resting in
  t.resting <- IntMap.remove id t.resting;
  t.emit (ts + t.cfg.ack_latency) (Event.Cancel_ack { id; accepted })

let touch_rule t ts r =
  match opposite_best t r.side with
  | Some (opx, _) ->
      let hit = match t.cfg.model with Touch -> not (better r.side opx r.px) | _ -> better r.side r.px opx in
      if hit then fill t r ts r.leaves
  | None -> ()

let queue_rule t ts r =
  match opposite_best t r.side with
  | Some (opx, _) when better r.side r.px opx -> fill t r ts r.leaves
  | Some (opx, _) when opx = r.px && r.ahead = Some 0 -> fill t r ts r.leaves
  | _ -> (
      match same_best t r.side with
      | Some (bpx, bsz) when bpx = r.px ->
          let ahead =
            match (r.ahead, r.level_size, t.cfg.model) with
            | None, _, _ -> bsz
            | Some a, Some prev, Queue_proportional ->
                let queue = prev - r.traded in
                let cancelled = queue - bsz in
                if cancelled > 0 && queue > 0 then a - (cancelled * a / queue) else a
            | Some a, _, _ -> a
          in
          r.ahead <- Some (max 0 (min ahead bsz));
          r.level_size <- Some bsz;
          r.traded <- 0
      | Some (bpx, _) when better r.side r.px bpx ->
          r.ahead <- Some 0;
          r.level_size <- None;
          r.traded <- 0
      | Some _ ->
          r.level_size <- None;
          r.traded <- 0
      | None ->
          r.ahead <- Some 0;
          r.level_size <- None;
          r.traded <- 0)

let on_quote t ts (q : Feed.quote) =
  t.bid <- (if q.bid_sz > 0 then Some (q.bid_px, q.bid_sz) else None);
  t.ask <- (if q.ask_sz > 0 then Some (q.ask_px, q.ask_sz) else None);
  let rule = if is_queue_model t then queue_rule else touch_rule in
  IntMap.iter (fun id _ -> match IntMap.find_opt id t.resting with Some r -> rule t ts r | None -> ()) t.resting

(* Trade prints matter only to the queue rules. *)
let on_trade t ts (tr : Feed.trade) =
  if is_queue_model t then
    let passive = opposite tr.aggressor in
    IntMap.iter
      (fun id _ ->
        match IntMap.find_opt id t.resting with
        | Some r when r.side = passive ->
            if better r.side r.px tr.px then fill t r ts r.leaves
            else if r.px = tr.px then begin
              let a = match r.ahead with Some a -> a | None -> max_int / 2 in
              r.ahead <- Some (max 0 (a - tr.sz));
              r.traded <- r.traded + tr.sz;
              if tr.sz > a then fill t r ts (tr.sz - a)
            end
        | _ -> ())
      t.resting
