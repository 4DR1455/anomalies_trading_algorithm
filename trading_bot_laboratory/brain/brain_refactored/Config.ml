open Types

let default_config = { 
  p_margin = 0.4; max_anomaly = 5.0; k = 2.0; alpha = 0.05; min_notional = 11.0;
  freeze_zone = -0.005; quit_zone = 0.001
}

let extract_float key str default =
  let search_str = "\"" ^ key ^ "\"" in
  let len = String.length str in
  let key_len = String.length search_str in
  let rec find_idx i =
    if i + key_len > len then None
    else if String.sub str i key_len = search_str then Some (i + key_len)
    else find_idx (i + 1)
  in
  match find_idx 0 with
  | None -> default
  | Some idx ->
      let rec find_num start_idx =
        if start_idx >= len then default
        else match str.[start_idx] with
          | ':' | ' ' | '\t' | '\n' | ',' | '"' -> find_num (start_idx + 1)
          | c when (c >= '0' && c <= '9') || c = '-' || c = '.' ->
              let rec read_num j =
                if j >= len then j
                else match str.[j] with
                  | c when (c >= '0' && c <= '9') || c = '-' || c = '.' -> read_num (j + 1)
                  | _ -> j
              in
              let end_idx = read_num start_idx in
              (try float_of_string (String.sub str start_idx (end_idx - start_idx))
               with _ -> default)
          | _ -> default
      in find_num idx

let load_config filename =
  try
    let ic = open_in filename in
    let rec read_all () = try let l = input_line ic in l ^ " " ^ read_all () with End_of_file -> "" in
    let content = read_all () in
    close_in ic;
    {
      p_margin = extract_float "p_margin" content default_config.p_margin;
      max_anomaly = extract_float "max_anomaly" content default_config.max_anomaly;
      k = extract_float "k" content default_config.k;
      alpha = extract_float "alpha" content default_config.alpha;
      min_notional = extract_float "min_notional" content default_config.min_notional;
      freeze_zone = extract_float "freeze_zone" content default_config.freeze_zone;
      quit_zone = extract_float "quit_zone" content default_config.quit_zone;
    }
  with e -> 
    let dir = Sys.getcwd () in
    let msg = Printexc.to_string e in
    print_endline ("[ALERTA CRÍTICA] Fallada a " ^ filename ^ " (" ^ dir ^ "): " ^ msg ^ ". Usant default (p_margin 0.01).");
    flush stdout;
    default_config
