(* The engine's flat key = value files (data/*.ini), read with the same rules as
   core/Config.hpp: '#' starts a comment anywhere on a line, keys and values are
   trimmed, the last occurrence of a key wins. *)

type t = (string, string) Hashtbl.t

let load path : t =
  let t = Hashtbl.create 32 in
  let ic = open_in path in
  Fun.protect
    ~finally:(fun () -> close_in ic)
    (fun () ->
      try
        while true do
          let line = input_line ic in
          let line = match String.index_opt line '#' with Some i -> String.sub line 0 i | None -> line in
          match String.index_opt line '=' with
          | None -> ()
          | Some i ->
              let k = String.trim (String.sub line 0 i) in
              let v = String.trim (String.sub line (i + 1) (String.length line - i - 1)) in
              if k <> "" then Hashtbl.replace t k v
        done
      with End_of_file -> ());
  t

let find t k = Hashtbl.find_opt t k

let str t k ~default = Option.value (find t k) ~default

let int t k ~default =
  match find t k with
  | None -> default
  | Some v -> ( try int_of_string v with Failure _ -> int_of_float (float_of_string v))

let float t k ~default = match find t k with None -> default | Some v -> float_of_string v

let bool t k ~default =
  match find t k with None -> default | Some v -> List.mem v [ "1"; "true"; "True"; "yes" ]
