(* Streaming readers for the two market data files.

   quotes  ts_ns,symbol,bid_px,bid_sz,ask_px,ask_sz   (the engine's feed format)
   trades  ts_ns,symbol,side,px,sz                     (side = aggressor, B or S)

   Both are read one line at a time, so a full ES session (8.4 million quotes)
   runs in constant memory. The files must be in timestamp order, which is how
   tools/databento_feed.py writes them; an out-of-order row stops the run rather
   than being silently re-sorted. *)

type quote = { ts : int; bid_px : int; bid_sz : int; ask_px : int; ask_sz : int }

type trade = { t_ts : int; aggressor : Types.side; px : int; sz : int }

type 'a reader = {
  ic : in_channel;
  path : string;
  parse : string list -> 'a;
  ts_of : 'a -> int;
  mutable next : 'a option;
  mutable line : int;
  mutable last_ts : int;
  mutable symbol : string option;
}

let fail r msg = failwith (Printf.sprintf "%s:%d: %s" r.path r.line msg)

let advance r =
  let rec read () =
    match input_line r.ic with
    | exception End_of_file -> None
    | "" ->
        r.line <- r.line + 1;
        read ()
    | s -> (
        r.line <- r.line + 1;
        let cols = String.split_on_char ',' (String.trim s) in
        (match cols with
        | _ :: sym :: _ -> (
            match r.symbol with
            | None -> r.symbol <- Some sym
            | Some s0 when s0 <> sym -> fail r (Printf.sprintf "second symbol %s (file has %s)" sym s0)
            | Some _ -> ())
        | _ -> ());
        match r.parse cols with
        | exception (Failure _ | Invalid_argument _) -> fail r "cannot parse row"
        | v ->
            let ts = r.ts_of v in
            if ts < r.last_ts then fail r "timestamps go backwards";
            r.last_ts <- ts;
            Some v)
  in
  r.next <- read ()

let open_reader path parse ts_of =
  let ic = open_in path in
  let r = { ic; path; parse; ts_of; next = None; line = 1; last_ts = min_int; symbol = None } in
  (match input_line ic with exception End_of_file -> () | _header -> ());
  advance r;
  r

let parse_quote = function
  | ts :: _sym :: bp :: bs :: ap :: az :: _ ->
      {
        ts = int_of_string ts;
        bid_px = int_of_string bp;
        bid_sz = int_of_string bs;
        ask_px = int_of_string ap;
        ask_sz = int_of_string az;
      }
  | _ -> failwith "quote row"

let parse_trade = function
  | ts :: _sym :: side :: px :: sz :: _ when String.length side > 0 ->
      { t_ts = int_of_string ts; aggressor = Types.side_of_char side.[0]; px = int_of_string px; sz = int_of_string sz }
  | _ -> failwith "trade row"

let quotes path = open_reader path parse_quote (fun q -> q.ts)

let trades path = open_reader path parse_trade (fun t -> t.t_ts)

let peek_ts r = match r.next with Some v -> r.ts_of v | None -> max_int

let take r =
  match r.next with
  | None -> invalid_arg "Feed.take: exhausted"
  | Some v ->
      advance r;
      v

let close r = close_in r.ic

let symbol r = r.symbol
