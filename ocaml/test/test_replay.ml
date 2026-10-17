(* The queue fill rules, position accounting, the event heap and the feed
   reader, one scenario at a time. *)

open Lobsim
open Types

let check name cond =
  if not cond then failwith ("FAILED: " ^ name);
  Printf.printf "ok  %s\n" name

let exchange model =
  let out = ref [] in
  let ex = Exchange.create { ack_latency = 0; fill_latency = 0; model } ~emit:(fun ts ev -> out := (ts, ev) :: !out) in
  let fills () =
    List.rev !out |> List.filter_map (function _, Event.Fill { id; px; qty; final } -> Some (id, px, qty, final) | _ -> None)
  in
  let clear () = out := [] in
  (ex, fills, clear)

let quote ex ts bp bs ap az = Exchange.on_quote ex ts { Feed.ts; bid_px = bp; bid_sz = bs; ask_px = ap; ask_sz = az }

let print ex ts aggressor px sz = Exchange.on_trade ex ts { Feed.t_ts = ts; aggressor; px; sz }

let queue_position () =
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 30 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:100 ~qty:10;
  check "a bid joining the touch queues behind the displayed 30" (Exchange.queue_ahead ex 1 = Some 30);
  print ex 3 Sell 100 20;
  check "a print smaller than the queue ahead does not fill" (fills () = [] && Exchange.queue_ahead ex 1 = Some 10);
  print ex 4 Sell 100 15;
  check "the part of a print that reaches the order fills it, partially" (fills () = [ (1, 100, 5, false) ]);
  print ex 5 Sell 100 10;
  check "the next print fills the rest" (fills () = [ (1, 100, 5, false); (1, 100, 5, true) ]);
  check "a filled order no longer rests" (Exchange.resting_count ex = 0)

let improving_order () =
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 30 103 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:101 ~qty:10;
  check "a bid inside the spread is alone at its price" (Exchange.queue_ahead ex 1 = Some 0);
  quote ex 3 100 30 102 30;
  check "an offer above it does not fill it" (fills () = []);
  quote ex 4 100 30 101 30;
  check "an offer at its price fills it" (fills () = [ (1, 101, 10, true) ])

let touch_is_optimistic () =
  let ex, fills, _ = exchange Exchange.Touch in
  quote ex 1 100 30 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:100 ~qty:10;
  print ex 3 Sell 100 20;
  check "touch ignores prints" (fills () = []);
  quote ex 4 99 10 100 5;
  check "touch fills the whole order once the offer reaches it" (fills () = [ (1, 100, 10, true) ]);
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 30 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:100 ~qty:10;
  print ex 3 Sell 100 20;
  quote ex 4 99 10 100 5;
  check "under the queue rule the same path fills nothing: 10 were still ahead when the level cleared"
    (fills () = [] && Exchange.queue_ahead ex 1 = Some 0);
  quote ex 5 99 10 100 5;
  check "after the level clears the order is first, and the offer at its price fills it" (fills () = [ (1, 100, 10, true) ])

let cancels () =
  let run model =
    let ex, _, _ = exchange model in
    quote ex 1 100 40 101 30;
    Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:100 ~qty:10;
    quote ex 3 100 60 101 30;
    quote ex 4 100 50 101 30;
    Exchange.queue_ahead ex 1
  in
  check "queue-behind: cancels are assumed to come from behind" (run Exchange.Queue_behind = Some 40);
  check "queue-proportional: 10 cancels from a queue of 60 with 40 ahead remove 6 ahead" (run Exchange.Queue_proportional = Some 34);
  let ex, _, _ = exchange Exchange.Queue_proportional in
  quote ex 1 100 40 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:100 ~qty:10;
  print ex 3 Sell 100 20;
  quote ex 4 100 20 101 30;
  check "a shrink fully explained by prints is not read as cancels" (Exchange.queue_ahead ex 1 = Some 20)

let trade_through () =
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 50 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:100 ~qty:10;
  print ex 3 Sell 99 5;
  check "a print below a resting bid fills it outright" (fills () = [ (1, 100, 10, true) ]);
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 50 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Sell ~px:101 ~qty:10;
  print ex 3 Sell 100 50;
  check "a sell-side print does not touch a resting offer" (fills () = [])

let behind_the_touch () =
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 30 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:99 ~qty:10;
  check "a bid below the touch has no known queue position" (Exchange.queue_ahead ex 1 = None);
  print ex 3 Sell 99 5;
  check "and a print at its price does not fill it" (fills () = []);
  quote ex 4 99 25 100 30;
  check "when its price becomes the best it queues behind everything shown" (Exchange.queue_ahead ex 1 = Some 25)

let marketable () =
  let ex, fills, _ = exchange Exchange.Queue_behind in
  quote ex 1 100 30 101 30;
  Exchange.on_new_order ex 2 ~id:1 ~side:Buy ~px:102 ~qty:10;
  check "a marketable order fills at the best offer and never rests" (fills () = [ (1, 101, 10, true) ] && Exchange.resting_count ex = 0)

let positions () =
  let pos = { Oms.net = 0; avg_px = 0.0; realized = 0.0 } in
  Oms.apply_fill pos Sell 100 20;
  Oms.apply_fill pos Buy 90 10;
  check "covering half a short realises +100 and keeps the entry at 100" (pos.net = -10 && pos.avg_px = 100.0 && pos.realized = 100.0);
  Oms.apply_fill pos Buy 95 25;
  check "a flip opens the remainder at the fill price" (pos.net = 15 && pos.avg_px = 95.0 && pos.realized = 150.0);
  Oms.apply_fill pos Buy 105 15;
  check "adding blends the entry price" (pos.net = 30 && pos.avg_px = 100.0)

let heap_ties () =
  let h = Heap.create () in
  List.iter (fun (ts, v) -> Heap.push h ts v) [ (5, "a"); (3, "b"); (5, "c"); (3, "d"); (1, "e"); (5, "f") ];
  let out = List.init 6 (fun _ -> snd (Heap.pop h)) in
  check "heap pops by time, then in push order" (out = [ "e"; "b"; "d"; "a"; "c"; "f" ])

let feed_order () =
  let path = Filename.temp_file "quotes" ".csv" in
  let oc = open_out path in
  output_string oc "ts_ns,symbol,bid_px,bid_sz,ask_px,ask_sz\n2,X,1,1,2,1\n1,X,1,1,2,1\n";
  close_out oc;
  let refused =
    match
      let r = Feed.quotes path in
      ignore (Feed.take r : Feed.quote);
      ignore (Feed.take r : Feed.quote)
    with
    | exception Failure _ -> true
    | () -> false
  in
  check "a feed going back in time is refused" refused

let () =
  queue_position ();
  improving_order ();
  touch_is_optimistic ();
  cancels ();
  trade_through ();
  behind_the_touch ();
  marketable ();
  positions ();
  heap_ties ();
  feed_order ()
