(* Throughput of the matching engine on random order flow: 60% limit orders
   within ten ticks of the touch, 30% cancels of a random resting order, 10%
   market orders.

     lobsim-bench [--ops N] [--seed S] *)

open Lobsim

let () =
  let ops = ref 2_000_000 and seed = ref 42 in
  Arg.parse [ ("--ops", Arg.Set_int ops, "N operations"); ("--seed", Arg.Set_int seed, "S seed") ] (fun _ -> ()) "lobsim-bench";
  Random.init !seed;
  let ob = Matching_engine.create () in
  let live = Array.make !ops 0 and n_live = ref 0 in
  let next_id = ref 1 and fills = ref 0 and volume = ref 0 in
  let mid = ref 10_000 in
  let t0 = Unix.gettimeofday () in
  for _ = 1 to !ops do
    let r = Random.int 100 in
    if r < 60 then begin
      let side = if Random.bool () then Types.Buy else Types.Sell in
      let off = Random.int 10 - 2 in
      let price = match side with Types.Buy -> !mid - off | Types.Sell -> !mid + off in
      let id = !next_id in
      incr next_id;
      let fs = Matching_engine.limit ob ~id ~side ~price ~qty:(1 + Random.int 20) in
      List.iter (fun (f : Matching_engine.fill) -> incr fills; volume := !volume + f.qty) fs;
      if Matching_engine.resting ob id <> None then begin
        live.(!n_live) <- id;
        incr n_live
      end
    end
    else if r < 90 && !n_live > 0 then begin
      let k = Random.int !n_live in
      ignore (Matching_engine.cancel ob live.(k));
      live.(k) <- live.(!n_live - 1);
      decr n_live
    end
    else begin
      let side = if Random.bool () then Types.Buy else Types.Sell in
      let id = !next_id in
      incr next_id;
      List.iter (fun (f : Matching_engine.fill) -> incr fills; volume := !volume + f.qty)
        (Matching_engine.market ob ~id ~side ~qty:(1 + Random.int 20))
    end;
    (match (Matching_engine.best_bid ob, Matching_engine.best_ask ob) with
    | Some (b, _), Some (a, _) -> mid := (a + b) / 2
    | _ -> ())
  done;
  let secs = Unix.gettimeofday () -. t0 in
  Printf.printf "operations   : %d\n" !ops;
  Printf.printf "fills        : %d (volume %d)\n" !fills !volume;
  Printf.printf "resting      : %d orders\n" (Matching_engine.order_count ob);
  Printf.printf "elapsed      : %.3f s\n" secs;
  Printf.printf "throughput   : %.2f M ops/s, %.0f ns/op\n" (float !ops /. secs /. 1e6) (secs /. float !ops *. 1e9)
