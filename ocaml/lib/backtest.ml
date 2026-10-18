(* The replay loop.

   Market data is streamed from the two files and merged with a heap of pending
   order traffic. Ordering follows the C++ driver, which seeds every quote into
   its event bus before anything else: at equal timestamps market data goes
   first, and order traffic keeps its push order. Trade prints (which the C++
   engine does not read) go before the quote with the same timestamp, the order
   in which CME publishes a trade and the book update it causes.

   Latency: strategy -> exchange on new orders and cancels, exchange -> strategy
   on acks, fills and cancel acks, plus the exchange's own ack and fill delays. *)

type config = {
  s2e_latency : int;
  e2s_latency : int;
  exchange : Exchange.config;
  mm : Market_maker.config;
  limits : Risk.limits;
}

type summary = {
  md_events : int;
  trade_prints : int;
  orders : int;
  fills : int;
  ending_position : int;
  last_mid : int option;
  realized : float;
  unrealized : float;
  max_drawdown : float;
  killed : bool;
}

let of_ini path =
  let c = Config.load path in
  let i k d = Config.int c k ~default:d and f k d = Config.float c k ~default:d in
  let limits =
    {
      Risk.max_position = i "max_position" 200;
      max_order_size = i "max_order_size" 100;
      max_open_qty = i "max_open_qty" 500;
      max_drawdown_ticks = f "max_drawdown_ticks" 1e12;
    }
  in
  let model = if Config.bool c "fill_on_touch" ~default:true then Exchange.Touch else Exchange.Through in
  ( {
      s2e_latency = i "strategy_to_exchange_latency_ns" 100_000;
      e2s_latency = i "exchange_to_strategy_latency_ns" 100_000;
      exchange = { ack_latency = i "ack_latency_ns" 50_000; fill_latency = i "fill_latency_ns" 50_000; model };
      mm =
        {
          quoting =
            (match Config.str c "quoting" ~default:"baseline" with
            | "baseline" -> Market_maker.Baseline
            | "micro" ->
                Market_maker.Micro
                  { k = i "inv_shift_ticks" 1; limit = i "inv_limit" 20; theta = f "imbalance_theta" 0.5 }
            | s -> invalid_arg ("unknown quoting rule: " ^ s));
          half_spread = i "half_spread_ticks" 2;
          min_spread = i "min_spread_ticks" 1;
          quote_size = i "quote_size" 10;
          max_position = limits.max_position;
          inv_skew = f "inv_skew" 0.01;
          requote_thresh = i "requote_thresh" 1;
        };
      limits;
    },
    Config.float c "tick_size" ~default:0.01 )

type sink = { on_fill : int -> Oms.order -> px:int -> qty:int -> unit; on_sample : int -> Oms.position -> int option -> float -> unit }

let no_sink = { on_fill = (fun _ _ ~px:_ ~qty:_ -> ()); on_sample = (fun _ _ _ _ -> ()) }

let run ?(sink = no_sink) cfg ~quotes ?trades () =
  let bus = Heap.create () in
  let oms = Oms.create () in
  let risk = Risk.create cfg.limits in
  let exch = Exchange.create cfg.exchange ~emit:(fun ts ev -> Heap.push bus (ts + cfg.e2s_latency) ev) in
  let mm = Market_maker.create cfg.mm oms risk ~send:(fun ts ev -> Heap.push bus (ts + cfg.s2e_latency) ev) in
  let mid = ref None in
  let peak = ref None and mdd = ref 0.0 in
  let md = ref 0 and prints = ref 0 in
  let sample ts =
    let u = match !mid with Some m -> Oms.unrealized oms.pos m | None -> 0.0 in
    let eq = oms.pos.realized +. u in
    let p = match !peak with None -> eq | Some p -> Float.max p eq in
    peak := Some p;
    mdd := Float.max !mdd (p -. eq);
    Risk.mark_equity risk eq;
    sink.on_sample ts oms.pos !mid u
  in
  let trade_ts () = match trades with Some r -> Feed.peek_ts r | None -> max_int in
  let rec loop () =
    let tq = Feed.peek_ts quotes and tt = trade_ts () and th = Heap.min_ts bus in
    if tq = max_int && tt = max_int && Heap.is_empty bus then ()
    else begin
      (if tt <= tq && tt <= th && tt <> max_int then begin
         let tr = Feed.take (Option.get trades) in
         incr prints;
         Exchange.on_trade exch tt tr
       end
       else if tq <= th && tq <> max_int then begin
         let (q : Feed.quote) = Feed.take quotes in
         incr md;
         mid := if q.bid_sz > 0 && q.ask_sz > 0 && q.bid_px < q.ask_px then Some ((q.bid_px + q.ask_px) / 2) else None;
         Exchange.on_quote exch tq q;
         Market_maker.on_quote mm tq q;
         sample tq
       end
       else
         let ts, ev = Heap.pop bus in
         match ev with
         | Event.New_order { id; side; px; qty } -> Exchange.on_new_order exch ts ~id ~side ~px ~qty
         | Event.Cancel_order { id } -> Exchange.on_cancel exch ts ~id
         | Event.Ack { id } -> Oms.on_ack oms id
         | Event.Cancel_ack { id; accepted } -> Oms.on_cancel_ack oms id ~accepted
         | Event.Fill { id; px; qty; final } -> (
             match Oms.on_fill oms id ~px ~qty ~final with
             | Some o ->
                 sink.on_fill ts o ~px ~qty;
                 sample ts
             | None -> ()));
      loop ()
    end
  in
  loop ();
  let unrealized = match !mid with Some m -> Oms.unrealized oms.pos m | None -> 0.0 in
  {
    md_events = !md;
    trade_prints = !prints;
    orders = oms.submitted;
    fills = oms.fills;
    ending_position = oms.pos.net;
    last_mid = !mid;
    realized = oms.pos.realized;
    unrealized;
    max_drawdown = !mdd;
    killed = risk.killed;
  }
