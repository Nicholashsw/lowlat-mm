(* Replay a top-of-book feed through the market maker under a chosen fill rule.

     lobsim-replay CONFIG QUOTES [--trades FILE] [--fill MODEL]
                   [--trades-out FILE] [--pnl-out FILE]

   CONFIG is one of the engine's data/*.ini files and QUOTES the feed the C++
   run_backtest reads. MODEL is touch, through, queue-behind or
   queue-proportional; without --fill the ini's fill_on_touch picks touch or
   through, as in the C++ engine. The queue rules need the trade prints that
   tools/databento_feed.py writes with --trades-out. *)

open Lobsim

let usage =
  "lobsim-replay CONFIG QUOTES [--trades FILE] [--fill MODEL] [--trades-out FILE] [--pnl-out FILE] [--micro K,LIMIT,THETA]"

let () =
  let trades = ref None and fill = ref None and trades_out = ref None and pnl_out = ref None in
  let micro = ref None in
  let positional = ref [] in
  let spec =
    [
      ("--trades", Arg.String (fun s -> trades := Some s), "FILE trade prints (ts_ns,symbol,side,px,sz)");
      ("--fill", Arg.String (fun s -> fill := Some (Exchange.model_of_string s)), "MODEL fill rule");
      ("--trades-out", Arg.String (fun s -> trades_out := Some s), "FILE write the fill blotter");
      ("--pnl-out", Arg.String (fun s -> pnl_out := Some s), "FILE write the PnL path");
      ( "--micro",
        Arg.String
          (fun s ->
            match String.split_on_char ',' s with
            | [ k; l; th ] -> micro := Some (int_of_string k, int_of_string l, float_of_string th)
            | _ -> raise (Arg.Bad "--micro K,LIMIT,THETA")),
        "K,LIMIT,THETA use the microprice quoting rule" );
    ]
  in
  Arg.parse spec (fun s -> positional := s :: !positional) usage;
  let ini, quotes_path =
    match List.rev !positional with
    | [ a; b ] -> (a, b)
    | _ ->
        prerr_endline usage;
        exit 2
  in
  let cfg, tick = Backtest.of_ini ini in
  let cfg = match !fill with Some m -> { cfg with exchange = { cfg.exchange with model = m } } | None -> cfg in
  let cfg =
    match !micro with
    | Some (k, limit, theta) -> { cfg with mm = { cfg.mm with quoting = Market_maker.Micro { k; limit; theta } } }
    | None -> cfg
  in
  (match (cfg.exchange.model, !trades) with
  | (Exchange.Queue_behind | Exchange.Queue_proportional), None ->
      prerr_endline "the queue fill rules need --trades";
      exit 2
  | _ -> ());
  let quotes = Feed.quotes quotes_path in
  let trades = Option.map Feed.trades !trades in
  let blotter = Option.map open_out !trades_out and pnl = Option.map open_out !pnl_out in
  Option.iter (fun oc -> output_string oc "ts_ns,order_id,side,px_ticks,qty\n") blotter;
  Option.iter (fun oc -> output_string oc "ts_ns,net_position,realized_pnl_ticks,unrealized_pnl_ticks,mid_ticks\n") pnl;
  let sink =
    {
      Backtest.on_fill =
        (fun ts (o : Oms.order) ~px ~qty ->
          Option.iter (fun oc -> Printf.fprintf oc "%d,%d,%c,%d,%d\n" ts o.id (Types.side_char o.side) px qty) blotter);
      on_sample =
        (fun ts pos mid u ->
          Option.iter
            (fun oc ->
              Printf.fprintf oc "%d,%d,%.4f,%.4f,%s\n" ts pos.Oms.net pos.realized u
                (match mid with Some m -> string_of_int m | None -> ""))
            pnl);
    }
  in
  let t0 = Unix.gettimeofday () in
  let s = Backtest.run ~sink cfg ~quotes ?trades () in
  let secs = Unix.gettimeofday () -. t0 in
  Option.iter close_out blotter;
  Option.iter close_out pnl;
  let sym = Option.value (Feed.symbol quotes) ~default:"?" in
  let total = s.realized +. s.unrealized in
  Printf.printf "# loaded %d md events for symbol %s\n\n" s.md_events sym;
  Printf.printf "=== Backtest summary ===\n";
  Printf.printf "symbol              : %s\n" sym;
  Printf.printf "fill model          : %s\n" (Exchange.string_of_model cfg.exchange.model);
  Printf.printf "md events           : %d\n" s.md_events;
  Printf.printf "trade prints        : %d\n" s.trade_prints;
  Printf.printf "orders submitted    : %d\n" s.orders;
  Printf.printf "fills               : %d\n" s.fills;
  Printf.printf "ending position     : %d\n" s.ending_position;
  Printf.printf "last mid (ticks)    : %s\n" (match s.last_mid with Some m -> string_of_int m | None -> "n/a");
  Printf.printf "realized PnL        : %.4f  (%.4f ticks)\n" (s.realized *. tick) s.realized;
  Printf.printf "unrealized PnL      : %.4f  (%.4f ticks)\n" (s.unrealized *. tick) s.unrealized;
  Printf.printf "total PnL           : %.4f  (%.4f ticks)\n" (total *. tick) total;
  Printf.printf "max drawdown        : %.4f  (%.4f ticks)\n" (s.max_drawdown *. tick) s.max_drawdown;
  Printf.printf "kill switch tripped : %s\n" (if s.killed then "YES" else "no");
  Printf.printf "replay time         : %.1f s\n" secs
