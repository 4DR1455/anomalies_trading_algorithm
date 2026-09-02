(* ========================================== *)
(* BRAIN 9: OMNI-ZONE SENSE REBALANCEIG AUTOMÀTIC *)
(* ========================================== *)

module FloatKey = struct
  type t = float
  let compare = Float.compare
end
module GridMap = Map.Make(FloatKey)

type config = {
  p_margin : float;
  max_anomaly : float;
  k : float;
  alpha : float;
  min_notional : float;
  freeze_zone : float;
  quit_zone : float;
}

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

let shares_amount max_r current_r max_shares level =
  if max_r = 0.0 then 0.0 else
    let x = current_r /. max_r in
    let x_clamped = max 0.0 (min 1.0 x) in
    let y = 1.0 -. (1.0 -. x_clamped ** level) ** (1.0 /. level) in
    max_shares *. y

let round3 f = (floor (f *. 1000.0 +. 0.5)) /. 1000.0

let rec trading_loop config_file prev_price momentum ema_ratio cooldown inventory cycles_horiz b_p b_m b_e b_cy mode =
  let cfg = load_config config_file in

  try
    let input_line_raw = input_line stdin in 
    let parts = String.split_on_char ' ' (String.trim input_line_raw) in
    
    match parts with
    | "INFO" :: data :: _ ->
        let data_parts = String.split_on_char ';' data in
        let (budget, current_price, total_shares) = match data_parts with
          | [b; p; s] -> (float_of_string b, float_of_string p, float_of_string s)
          | _ -> (0.0, 0.0, 0.0)
        in

        let current_ratio = if prev_price = 0.0 then 1.0 else current_price /. prev_price in
        let new_momentum = momentum *. current_ratio in
        let new_ema_ratio = if ema_ratio = 0.0 then momentum else (momentum *. cfg.alpha) +. (ema_ratio *. (1.0 -. cfg.alpha)) in
        
        let diff = (new_momentum /. new_ema_ratio) -. 1.0 in
        let dist_anomaly = abs_float (diff *. 100.0) in

        let gmr = new_ema_ratio -. ema_ratio in

        (* Nova Lògica de Transicions (Sense rebalanceig) *)
        let (next_mode, transition_happened) = 
          if mode = 0 then
            if gmr <= cfg.freeze_zone then (print_endline "CMD FREEZE"; flush stdout; (1, true))
            else (0, false)
          else 
            if gmr > cfg.quit_zone then 
              (print_endline "CMD UNFREEZE"; flush stdout; (0, true))
            else (1, false)
        in

        if transition_happened then
           trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 current_price new_momentum new_ema_ratio cycles_horiz next_mode
        else

        if cooldown > 0 then
          (print_endline "HOLD 0 0"; flush stdout; 
           trading_loop config_file current_price new_momentum new_ema_ratio (cooldown - 1) inventory (cycles_horiz + 1) b_p b_m b_e b_cy mode)
        
        else if dist_anomaly > cfg.max_anomaly then
            (print_endline "HOLD 0 0"; flush stdout; 
             trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 current_price new_momentum new_ema_ratio cycles_horiz mode)
      
        else if mode = 0 then
          let is_grid_occupied = false in 
          
          if diff < 0.0 && dist_anomaly > cfg.p_margin then
            let max_qty = budget /. current_price in
            let final_q = round3 (shares_amount cfg.max_anomaly dist_anomaly max_qty cfg.k) in
            
            if final_q *. current_price > cfg.min_notional && dist_anomaly < cfg.max_anomaly then
              let entry_r = new_momentum *. 1.0001 in
              let exit_r = new_ema_ratio *. 0.9999 in
              (Printf.printf "VALL %.4f %.4f %.4f\n" final_q entry_r exit_r; flush stdout;
               trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz mode)
            else
              (print_endline "HOLD 0 0"; flush stdout;
               trading_loop config_file current_price new_momentum new_ema_ratio cooldown inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode)
          
          else if diff > 0.0 && dist_anomaly > cfg.p_margin then
            let final_q = round3 (shares_amount cfg.max_anomaly dist_anomaly total_shares cfg.k) in
            
            if final_q *. current_price > cfg.min_notional && dist_anomaly < cfg.max_anomaly then
              let entry_r = new_momentum *. 0.9999 in
              let exit_r = new_ema_ratio *. 1.0001 in
              (Printf.printf "PIC %.4f %.4f %.4f\n" final_q entry_r exit_r; flush stdout;
               trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz mode)
            else
              (print_endline "HOLD 0 0"; flush stdout;
               trading_loop config_file current_price new_momentum new_ema_ratio cooldown inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode)
               
          else if cycles_horiz >= 60 && (not is_grid_occupied) && budget > (cfg.min_notional *. 1.5) then
            let qty = round3 ((budget *. 0.25) /. current_price) in
            let entry_r = new_momentum *. 1.0001 in
            let exit_r = new_momentum *. (1.0 +. (cfg.p_margin /. 100.0)) in
            (Printf.printf "VALL %.4f %.4f %.4f\n" qty entry_r exit_r; flush stdout;
             trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz mode)
             
          else
            (print_endline "HOLD 0 0"; flush stdout; 
             trading_loop config_file current_price new_momentum new_ema_ratio 0 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode)
        
        else
            (print_endline "HOLD 0 0"; flush stdout; 
             trading_loop config_file current_price new_momentum new_ema_ratio 0 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode)

    | "BOUGHT" :: id_str :: _ ->
        let id = float_of_string id_str in
        let new_inv = GridMap.add id true inventory in
        trading_loop config_file prev_price momentum ema_ratio 45 new_inv 0 b_p b_m b_e b_cy mode

    | "SOLD" :: id_str :: _ ->
        let id = float_of_string id_str in
        let new_inv = GridMap.add id false inventory in
        trading_loop config_file prev_price momentum ema_ratio 0 new_inv cycles_horiz b_p b_m b_e b_cy mode

    | "ROLLBACK" :: _ ->
        trading_loop config_file b_p b_m b_e 0 inventory b_cy b_p b_m b_e b_cy mode

    | _ -> trading_loop config_file prev_price momentum ema_ratio cooldown inventory (cycles_horiz + 1) b_p b_m b_e cycles_horiz mode

  with End_of_file -> () 

(* L'últim paràmetre ara és '0' en lloc de '-1' per saltar directament al mode operatiu *)
let () = trading_loop "params.json" 0.0 1.0 0.0 45 GridMap.empty 0 0.0 1.0 0.0 0 0
