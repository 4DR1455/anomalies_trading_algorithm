(* ========================================== *)
(* BRAIN 8.4: SNIPER WITH RISK CONTROL        *)
(* ========================================== *)

(* Define a Float key for the Map to track grid positions *)
module FloatKey = struct
  type t = float
  let compare = Float.compare
end
module GridMap = Map.Make(FloatKey)

(* Configuration parameters loaded from params.txt *)
type config = {
  p_margin     : float; (* Profit margin and Grid spacing width *)
  max_anomaly  : float; (* Maximum allowed anomaly deviation (Circuit Breaker) *)
  k            : float; (* Convexity exponent for the capital allocation curve *)
  alpha        : float; (* Smoothing factor for the EMA calculation *)
  min_notional : float; (* Minimum order value allowed (in USD) *)
}

(* Default fallback configuration *)
let default_config = { 
  p_margin = 0.01; 
  max_anomaly = 5.0; 
  k = 2.0; 
  alpha = 0.05; 
  min_notional = 11.0 
}

(* Helper function to parse configuration from file *)
let load_config filename =
  try
    let ic = open_in filename in
    let line = input_line ic in
    close_in ic;
    let parts = String.split_on_char ' ' (String.trim line) in
    match parts with
    | p_m :: max_a :: k_val :: alpha_s :: min_n :: _ ->
        { p_margin = float_of_string p_m; 
          max_anomaly = float_of_string max_a; 
          k = float_of_string k_val;
          alpha = float_of_string alpha_s; 
          min_notional = float_of_string min_n }
    | _ -> default_config
  with _ -> default_config

(* Asymptotic Capital Allocation Formula:
   Calculates the % of max shares to buy based on the severity of the anomaly.
   Formula: y = 1 - (1 - x^k)^(1/k)
*)
let shares_amount max_r current_r max_shares level =
  if max_r = 0.0 then 0.0 else
    let x = current_r /. max_r in
    let x_clamped = max 0.0 (min 1.0 x) in
    let y = 1.0 -. (1.0 -. x_clamped ** level) ** (1.0 /. level) in
    max_shares *. y

(* Round to 3 decimal places *)
let round3 f = (floor (f *. 1000.0 +. 0.5)) /. 1000.0

(* Main Recursive Trading Loop 
   - Reads market data from STDIN (piped from C++ engine)
   - Calculates Momentum and EMA
   - Decides whether to BUY, HOLD, or manage inventory
*)
let rec trading_loop config_file prev_price momentum ema_ratio cooldown inventory cycles_horiz b_p b_m b_e b_cy =
  let cfg = load_config config_file in
  let grid_size = cfg.p_margin *. 2.0 in

  try
    let input_line_raw = input_line stdin in 
    let parts = String.split_on_char ' ' (String.trim input_line_raw) in
    
    match parts with
    (* --- PROCESS MARKET DATA --- *)
    | "INFO" :: data :: _ ->
        let data_parts = String.split_on_char ';' data in
        let (budget, current_price, _total_shares) = match data_parts with
          | [b; p; s] -> (float_of_string b, float_of_string p, float_of_string s)
          | _ -> (0.0, 0.0, 0.0)
        in

        (* Calculate Indicators *)
        let current_ratio = if prev_price = 0.0 then 1.0 else current_price /. prev_price in
        let new_momentum = momentum *. current_ratio in
        let new_ema_ratio = if ema_ratio = 0.0 then momentum else (momentum *. cfg.alpha) +. (ema_ratio *. (1.0 -. cfg.alpha)) in
        
        (* Grid Calculation: Keep grid_id as float to avoid string->int conversion errors *)
        let current_grid_id = new_momentum /. grid_size in
        let is_grid_occupied = try GridMap.find current_grid_id inventory with Not_found -> false in

        (* Anomaly Detection *)
        let diff = (current_ratio /. new_ema_ratio) -. 1.0 in
        let dist_anomaly = abs_float (diff *. 100.0) in

        (* --- DECISION LOGIC --- *)
        
        (* 1. Cooldown active: Wait before next trade *)
        if cooldown > 0 then
          (print_endline "HOLD 0 0"; flush stdout; 
           trading_loop config_file current_price new_momentum new_ema_ratio (cooldown - 1) inventory (cycles_horiz + 1) b_p b_m b_e b_cy)
        
        (* 2. Circuit Breaker: Anomaly too extreme, likely data error or crash. Hold & Reset history. *)
        else if dist_anomaly > cfg.max_anomaly then
            (print_endline "HOLD 0 0"; flush stdout; 
             trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 current_price new_momentum new_ema_ratio cycles_horiz)
          
        (* 3. SNIPER ENTRY: Undervalued anomaly detected (Mean Reversion) *)
          else if diff < 0.0 && dist_anomaly > cfg.p_margin then
            let max_qty = budget /. current_price in
            let final_q = round3 (shares_amount cfg.max_anomaly dist_anomaly max_qty cfg.k) in
            
            if final_q *. current_price > cfg.min_notional && dist_anomaly < cfg.max_anomaly then
              (* OUTPUT: BUY <Quantity> <TargetMomentum> *)
              (Printf.printf "BUY %.4f %.4f\n" final_q new_ema_ratio; flush stdout;
               trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz)
            else
              (print_endline "HOLD 0 0"; flush stdout;
           trading_loop config_file current_price new_momentum new_ema_ratio cooldown inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1))

          (* 4. FORCED TURNOVER: Time-based entry (Impatience Mechanism) *)
          (* If market is flat for too long (60 cycles), force a small buy to churn inventory *)
          else if cycles_horiz >= 60 && (not is_grid_occupied) && budget > (cfg.min_notional *. 1.5) then
            let qty = round3 ((budget *. 0.025) /. current_price) in
            (Printf.printf "BUY %.4f %.4f\n" qty (new_momentum +. (cfg.p_margin /. 100.0)); flush stdout;
             trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz)
          
          (* 5. Default: No signal, Hold position *)
          else
            (print_endline "HOLD 0 0"; flush stdout; 
             trading_loop config_file current_price new_momentum new_ema_ratio 0 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1))

    (* --- STATE MANAGEMENT MESSAGES --- *)
    
    (* Mark grid as occupied after successful buy *)
    | "BOUGHT" :: id_str :: _ ->
        let id = float_of_string id_str in
        let new_inv = GridMap.add id true inventory in
        trading_loop config_file prev_price momentum ema_ratio 45 new_inv 0 b_p b_m b_e b_cy

    (* Mark grid as free after successful sell *)
    | "SOLD" :: id_str :: _ ->
        let id = float_of_string id_str in
        let new_inv = GridMap.add id false inventory in
        trading_loop config_file prev_price momentum ema_ratio 0 new_inv cycles_horiz b_p b_m b_e b_cy

    (* Rollback state if C++ execution failed *)
    | "ROLLBACK" :: _ ->
        trading_loop config_file b_p b_m b_e 0 inventory b_cy b_p b_m b_e b_cy

    (* Ignore unknown messages *)
    | _ -> trading_loop config_file prev_price momentum ema_ratio cooldown inventory (cycles_horiz + 1) b_p b_m b_e cycles_horiz

  with End_of_file -> () 

(* Entry point: Start loop with initial state *)
let () = trading_loop "params.txt" 0.0 1.0 0.0 45 GridMap.empty 0 0.0 1.0 0.0 0