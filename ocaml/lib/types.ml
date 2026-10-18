type side = Buy | Sell

let side_char = function Buy -> 'B' | Sell -> 'S'

let opposite = function Buy -> Sell | Sell -> Buy

let side_of_char = function
  | 'B' | 'b' -> Buy
  | 'S' | 's' | 'A' | 'a' -> Sell
  | c -> invalid_arg (Printf.sprintf "side_of_char: %C" c)
