(* Binary min-heap ordered on (timestamp, sequence). The sequence number is the
   insertion count, so events scheduled for the same nanosecond come out in the
   order they were pushed. That tie rule is what makes a replay deterministic,
   and it is the same rule as the C++ EventBus. *)

type 'a entry = { ts : int; seq : int; v : 'a }

type 'a t = { mutable data : 'a entry array; mutable size : int; mutable next_seq : int }

let create () = { data = [||]; size = 0; next_seq = 0 }

let is_empty h = h.size = 0

let length h = h.size

let before a b = a.ts < b.ts || (a.ts = b.ts && a.seq < b.seq)

let swap d i j =
  let t = d.(i) in
  d.(i) <- d.(j);
  d.(j) <- t

let push h ts v =
  let e = { ts; seq = h.next_seq; v } in
  h.next_seq <- h.next_seq + 1;
  if h.size = Array.length h.data then begin
    let d = Array.make (max 16 (2 * h.size)) e in
    Array.blit h.data 0 d 0 h.size;
    h.data <- d
  end;
  let d = h.data in
  d.(h.size) <- e;
  let i = ref h.size in
  h.size <- h.size + 1;
  while !i > 0 && before d.(!i) d.((!i - 1) / 2) do
    let p = (!i - 1) / 2 in
    swap d !i p;
    i := p
  done

(* Timestamp of the next event, or [max_int] when empty. *)
let min_ts h = if h.size = 0 then max_int else h.data.(0).ts

let pop h =
  if h.size = 0 then invalid_arg "Heap.pop: empty";
  let d = h.data in
  let top = d.(0) in
  h.size <- h.size - 1;
  if h.size > 0 then begin
    d.(0) <- d.(h.size);
    let i = ref 0 and settled = ref false in
    while not !settled do
      let l = (2 * !i) + 1 in
      let m = ref !i in
      if l < h.size && before d.(l) d.(!m) then m := l;
      if l + 1 < h.size && before d.(l + 1) d.(!m) then m := l + 1;
      if !m = !i then settled := true
      else begin
        swap d !i !m;
        i := !m
      end
    done
  end;
  (top.ts, top.v)
