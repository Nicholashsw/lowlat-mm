(* Messages between the strategy side and the simulated exchange. Each carries
   only what its receiver needs; the timestamp lives in the event queue. *)

type t =
  | New_order of { id : int; side : Types.side; px : int; qty : int }
  | Cancel_order of { id : int }
  | Ack of { id : int }
  | Fill of { id : int; px : int; qty : int; final : bool }
  | Cancel_ack of { id : int; accepted : bool }
