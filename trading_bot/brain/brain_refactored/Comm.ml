type incoming =
  | Info of float * float * float
  | Bought of float
  | Sold of float
  | Rollback
  | Unknown
  | EOF

let read_message () =
  try
    let line = input_line stdin in
    let parts = String.split_on_char ' ' (String.trim line) in
    match parts with
    | "INFO" :: data :: _ ->
        let data_parts = String.split_on_char ';' data in
        (match data_parts with
         | [b; p; s] -> Info (float_of_string b, float_of_string p, float_of_string s)
         | _ -> Info (0.0, 0.0, 0.0))
    | "BOUGHT" :: id_str :: _ -> Bought (float_of_string id_str)
    | "SOLD" :: id_str :: _ -> Sold (float_of_string id_str)
    | "ROLLBACK" :: _ -> Rollback
    | _ -> Unknown
  with End_of_file -> EOF

let send_hold () =
  print_endline "HOLD 0 0";
  flush stdout

let send_buy qty target =
  Printf.printf "BUY %.4f %.4f\n" qty target;
  flush stdout