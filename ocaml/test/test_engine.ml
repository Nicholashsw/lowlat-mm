(* The matching engine against a deliberately naive reference: a list of resting
   orders, re-sorted by price and arrival on every incoming order. Random order
   flow must produce the same fills, in the same order, and the same book. *)

open Lobsim
open Types
module ME = Matching_engine

module Reference = struct
  type o = { id : int; side : side; price : int; mutable qty : int; seq : int }

  type t = { mutable orders : o list; mutable seq : int }

  let create () = { orders = []; seq = 0 }

  let incoming t ~id ~side ~limit ~qty =
    let crosses o =
      o.side <> side && match (side, limit) with Buy, Some p -> o.price <= p | Sell, Some p -> o.price >= p | _, None -> true
    in
    let priority a b =
      let c = match side with Buy -> compare a.price b.price | Sell -> compare b.price a.price in
      if c <> 0 then c else compare a.seq b.seq
    in
    let rec go qty acc = function
      | o :: rest when qty > 0 ->
          let q = min qty o.qty in
          o.qty <- o.qty - q;
          go (qty - q) ((o.id, id, o.price, q) :: acc) rest
      | _ -> (qty, acc)
    in
    let left, fills = go qty [] (List.sort priority (List.filter crosses t.orders)) in
    t.orders <- List.filter (fun o -> o.qty > 0) t.orders;
    (match limit with
    | Some price when left > 0 ->
        t.orders <- t.orders @ [ { id; side; price; qty = left; seq = t.seq } ];
        t.seq <- t.seq + 1
    | _ -> ());
    List.rev fills

  let cancel t id =
    let found = List.exists (fun o -> o.id = id) t.orders in
    t.orders <- List.filter (fun o -> o.id <> id) t.orders;
    found

  let reduce t id by =
    match List.find_opt (fun o -> o.id = id) t.orders with
    | None -> false
    | Some o ->
        o.qty <- o.qty - min by o.qty;
        t.orders <- List.filter (fun o -> o.qty > 0) t.orders;
        true

  let depth t side =
    let tbl = Hashtbl.create 8 in
    List.iter
      (fun o -> if o.side = side then Hashtbl.replace tbl o.price (o.qty + Option.value (Hashtbl.find_opt tbl o.price) ~default:0))
      t.orders;
    let l = List.sort compare (Hashtbl.fold (fun p q acc -> (p, q) :: acc) tbl []) in
    match side with Buy -> List.rev l | Sell -> l
end

type op = Limit of side * int * int | Market of side * int | Cancel of int | Reduce of int * int

let show_op = function
  | Limit (s, p, q) -> Printf.sprintf "limit %c %d@%d" (side_char s) q p
  | Market (s, q) -> Printf.sprintf "market %c %d" (side_char s) q
  | Cancel k -> Printf.sprintf "cancel #%d" k
  | Reduce (k, b) -> Printf.sprintf "reduce #%d by %d" k b

let gen_op =
  let open QCheck.Gen in
  let side = oneofl [ Buy; Sell ] in
  frequency
    [
      (6, map3 (fun s p q -> Limit (s, p, q)) side (int_range 95 105) (int_range 1 10));
      (1, map2 (fun s q -> Market (s, q)) side (int_range 1 25));
      (2, map (fun k -> Cancel k) (int_range 0 60));
      (1, map2 (fun k b -> Reduce (k, b)) (int_range 0 60) (int_range 1 5));
    ]

let arb_ops = QCheck.make ~print:(QCheck.Print.list show_op) ~shrink:QCheck.Shrink.list QCheck.Gen.(list_size (int_range 1 120) gen_op)

let fills_of = List.map (fun (f : ME.fill) -> (f.maker, f.taker, f.price, f.qty))

(* Run ops through both books, calling [check] after every step. Ids are
   1, 2, 3, ... in submission order; Cancel/Reduce pick from those issued. *)
let replay ops check =
  let ob = ME.create () and rf = Reference.create () in
  let next = ref 1 in
  List.for_all
    (fun op ->
      let id_of k = if !next = 1 then 0 else 1 + (k mod (!next - 1)) in
      let a, b =
        match op with
        | Limit (side, price, qty) ->
            let id = !next in
            incr next;
            (fills_of (ME.limit ob ~id ~side ~price ~qty), Reference.incoming rf ~id ~side ~limit:(Some price) ~qty)
        | Market (side, qty) ->
            let id = !next in
            incr next;
            (fills_of (ME.market ob ~id ~side ~qty), Reference.incoming rf ~id ~side ~limit:None ~qty)
        | Cancel k ->
            let id = id_of k in
            assert (ME.cancel ob id = Reference.cancel rf id);
            ([], [])
        | Reduce (k, by) ->
            let id = id_of k in
            assert (ME.reduce ob id ~by = Reference.reduce rf id by);
            ([], [])
      in
      check op ob rf a b)
    ops

let same_as_reference =
  QCheck.Test.make ~name:"fills and book match the reference" ~count:2000 arb_ops (fun ops ->
      replay ops (fun _ ob rf a b ->
          a = b && ME.depth ob Buy 100 = Reference.depth rf Buy && ME.depth ob Sell 100 = Reference.depth rf Sell))

let never_crossed =
  QCheck.Test.make ~name:"book is never crossed" ~count:2000 arb_ops (fun ops ->
      replay ops (fun _ ob _ _ _ ->
          match (ME.best_bid ob, ME.best_ask ob) with Some (b, _), Some (a, _) -> b < a | _ -> true))

let fills_respect_limits =
  QCheck.Test.make ~name:"fills respect the limit and walk the book outwards" ~count:2000 arb_ops (fun ops ->
      replay ops (fun op _ _ fills _ ->
          let prices = List.map (fun (_, _, p, _) -> p) fills in
          match op with
          | Limit (Buy, lim, _) -> List.for_all (fun p -> p <= lim) prices && prices = List.sort compare prices
          | Limit (Sell, lim, _) -> List.for_all (fun p -> p >= lim) prices && prices = List.rev (List.sort compare prices)
          | Market (Buy, _) -> prices = List.sort compare prices
          | Market (Sell, _) -> prices = List.rev (List.sort compare prices)
          | _ -> true))

let quantity_conserved =
  QCheck.Test.make ~name:"quantity is conserved" ~count:2000 arb_ops (fun ops ->
      let ob = ME.create () in
      let submitted = ref 0 and traded = ref 0 and removed = ref 0 and next = ref 1 in
      List.iter
        (fun op ->
          match op with
          | Limit (side, price, qty) ->
              submitted := !submitted + qty;
              traded := !traded + (2 * List.fold_left (fun a (f : ME.fill) -> a + f.qty) 0 (ME.limit ob ~id:!next ~side ~price ~qty));
              incr next
          | Market (side, qty) ->
              let fs = ME.market ob ~id:!next ~side ~qty in
              let q = List.fold_left (fun a (f : ME.fill) -> a + f.qty) 0 fs in
              submitted := !submitted + q;
              traded := !traded + (2 * q);
              incr next
          | Cancel k when !next > 1 ->
              let id = 1 + (k mod (!next - 1)) in
              (match ME.resting ob id with Some (_, _, q) -> removed := !removed + q | None -> ());
              ignore (ME.cancel ob id)
          | Reduce (k, by) when !next > 1 ->
              let id = 1 + (k mod (!next - 1)) in
              (match ME.resting ob id with Some (_, _, q) -> removed := !removed + min q by | None -> ());
              ignore (ME.reduce ob id ~by)
          | _ -> ())
        ops;
      let resting side = List.fold_left (fun a (_, q) -> a + q) 0 (ME.depth ob side 1000) in
      !submitted = !traded + !removed + resting Buy + resting Sell)

(* A few hand-written cases that read as documentation. *)
let unit_cases () =
  let ob = ME.create () in
  ignore (ME.limit ob ~id:1 ~side:Sell ~price:101 ~qty:5);
  ignore (ME.limit ob ~id:2 ~side:Sell ~price:101 ~qty:5);
  ignore (ME.limit ob ~id:3 ~side:Sell ~price:102 ~qty:5);
  assert (ME.queue ob Sell 101 = [ (1, 5); (2, 5) ]);
  let f = fills_of (ME.limit ob ~id:4 ~side:Buy ~price:102 ~qty:12) in
  assert (f = [ (1, 4, 101, 5); (2, 4, 101, 5); (3, 4, 102, 2) ]);
  assert (ME.best_ask ob = Some (102, 3));
  assert (ME.best_bid ob = None);
  (* reduce keeps priority, cancel removes *)
  ignore (ME.limit ob ~id:5 ~side:Buy ~price:100 ~qty:10);
  ignore (ME.limit ob ~id:6 ~side:Buy ~price:100 ~qty:10);
  assert (ME.reduce ob 5 ~by:7);
  assert (ME.queue ob Buy 100 = [ (5, 3); (6, 10) ]);
  assert (ME.cancel ob 5 && not (ME.cancel ob 5));
  assert (fills_of (ME.market ob ~id:7 ~side:Sell ~qty:4) = [ (6, 7, 100, 4) ]);
  (* duplicate resting id and zero size are rejected *)
  assert (try ignore (ME.limit ob ~id:6 ~side:Buy ~price:99 ~qty:1); false with Invalid_argument _ -> true);
  assert (try ignore (ME.limit ob ~id:8 ~side:Buy ~price:99 ~qty:0); false with Invalid_argument _ -> true);
  print_endline "matching engine unit cases: ok"

let () =
  unit_cases ();
  exit (QCheck_base_runner.run_tests ~verbose:true [ same_as_reference; never_crossed; fills_respect_limits; quantity_conserved ])
