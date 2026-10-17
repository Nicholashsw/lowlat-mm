(* Write a synthetic top-of-book feed and matching trade prints, for tests and
   for checking this replay against the C++ engine. Nothing here resembles a real
   session beyond the mechanics: prints deplete the touch, an emptied level moves
   the price, the odd sweep clears several levels, and sizes come and go.

     lobsim-gen-feed --events N --seed S --quotes FILE --trades FILE *)

let () =
  let n = ref 20_000 and seed = ref 7 and quotes = ref "synthetic_feed.csv" and trades = ref "synthetic_trades.csv" in
  Arg.parse
    [
      ("--events", Arg.Set_int n, "N events");
      ("--seed", Arg.Set_int seed, "S random seed");
      ("--quotes", Arg.Set_string quotes, "FILE quotes out");
      ("--trades", Arg.Set_string trades, "FILE trades out");
    ]
    (fun _ -> ())
    "lobsim-gen-feed --events N --seed S --quotes FILE --trades FILE";
  Random.init !seed;
  let qo = open_out !quotes and tro = open_out !trades in
  output_string qo "ts_ns,symbol,bid_px,bid_sz,ask_px,ask_sz\n";
  output_string tro "ts_ns,symbol,side,px,sz\n";
  let ts = ref 1_760_620_200_000_000_000 in
  let bid = ref 26_000 and ask = ref 26_001 and bsz = ref 25 and asz = ref 25 in
  let fresh () = 5 + Random.int 40 in
  let quote () = Printf.fprintf qo "%d,SYN,%d,%d,%d,%d\n" !ts !bid !bsz !ask !asz in
  let print side px sz = Printf.fprintf tro "%d,SYN,%c,%d,%d\n" !ts side px sz in
  quote ();
  for _ = 1 to !n do
    ts := !ts + 1_000 + Random.int 3_000_000;
    let r = Random.float 1.0 in
    if r < 0.04 then begin
      (* a sweep: an aggressor clears several levels and the book reprices *)
      let k = 1 + Random.int 4 in
      if Random.bool () then begin
        for i = 0 to k - 1 do print 'B' (!ask + i) (if i = 0 then !asz else fresh ()) done;
        ask := !ask + k;
        bid := !ask - 1 - Random.int 2
      end
      else begin
        for i = 0 to k - 1 do print 'S' (!bid - i) (if i = 0 then !bsz else fresh ()) done;
        bid := !bid - k;
        ask := !bid + 1 + Random.int 2
      end;
      bsz := fresh ();
      asz := fresh ()
    end
    else if r < 0.3 then begin
      let sz = 1 + Random.int (min !bsz 20) in
      print 'S' !bid sz;
      bsz := !bsz - sz;
      if !bsz = 0 then begin
        decr bid;
        bsz := fresh ()
      end
    end
    else if r < 0.6 then begin
      let sz = 1 + Random.int (min !asz 20) in
      print 'B' !ask sz;
      asz := !asz - sz;
      if !asz = 0 then begin
        incr ask;
        asz := fresh ()
      end
    end
    else if r < 0.85 then begin
      let d = Random.int 21 - 10 in
      if Random.bool () then bsz := max 1 (!bsz + d) else asz := max 1 (!asz + d)
    end
    else if !ask - !bid > 1 then begin
      if Random.bool () then (incr bid; bsz := 1 + Random.int 10) else (decr ask; asz := 1 + Random.int 10)
    end
    else if Random.bool () then (decr bid; bsz := fresh ())
    else (incr ask; asz := fresh ());
    quote ()
  done;
  close_out qo;
  close_out tro
