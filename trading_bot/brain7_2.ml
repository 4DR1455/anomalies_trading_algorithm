(* ========================================== *)
(* FINAL BRAIN: Rollback + $12 Min Filter     *)
(* ========================================== *)

(* --- DYNAMIC CONFIGURATION --- *)
type config = {
  min_margin : float;
  max_margin : float;
  alpha : float;
  buy_level : float;
  sell_level : float;
  min_notional : float;
}

(* Default values *)
let default_config = { 
  min_margin = 1.0; 
  max_margin = 5.0; 
  alpha = 0.015;
  buy_level = 1000.0; 
  sell_level = 5.0;
  min_notional = 11.0
}

let load_config filename =
  try
    let ic = open_in filename in
    let line = input_line ic in
    close_in ic;
    let parts = String.split_on_char ' ' line in
    match parts with
    | min_s :: max_s :: alpha_s :: buy_l :: sell_l :: min_n :: _ ->
        { 
          min_margin = float_of_string min_s;
          max_margin = float_of_string max_s;
          alpha = float_of_string alpha_s;
          buy_level = float_of_string buy_l;
          sell_level = float_of_string sell_l;
          min_notional = float_of_string min_n
        }
    (* Backwards compatibility *)
    | min_s :: max_s :: alpha_s :: buy_l :: sell_l :: _ ->
        { 
          min_margin = float_of_string min_s;
          max_margin = float_of_string max_s;
          alpha = float_of_string alpha_s;
          buy_level = float_of_string buy_l;
          sell_level = float_of_string sell_l;
          min_notional = 10.0
        }
    | _ -> default_config
  with _ -> default_config

(* --- Structures --- *)
module PriceMap = Map.Make(Int)

(* --- Parsing --- *)
let parse_line line =
  let parts = String.split_on_char ' ' line in
  match parts with
  | "INFO" :: data :: _ ->
      let data_parts = String.split_on_char ';' data in
      (match data_parts with
       | [budget; price; shares] -> 
           (try Some ("INFO", float_of_string budget, float_of_string price, float_of_string shares)
            with _ -> None)
       | _ -> None)
  | "BOUGHT" :: qty_str :: price_str :: _ ->
      (try Some ("BOUGHT", 0.0, float_of_string price_str, float_of_string qty_str)
       with _ -> None)
  | "SOLD" :: qty_str :: price_str :: _ ->
      (try Some ("SOLD", 0.0, float_of_string price_str, float_of_string qty_str)
       with _ -> None)
  | "ROLLBACK" :: _ -> 
      Some ("ROLLBACK", 0.0, 0.0, 0.0)
  | _ -> None

(* --- Math Helpers --- *)
let shares_amount max_r current_r max_shares level =
  if max_r = 0.0 then 0.0
  else
    let x = current_r /. max_r in
    let x_clamped = max 0.0 (min 1.0 x) in
    let y = 1.0 -. (1.0 -. x_clamped ** level) ** (1.0 /. level) in
    max_shares *. y

let round3 f = (floor (f *. 1000.0 +. 0.5)) /. 1000.0

(* --- MAIN TRADING LOOP --- *)
let rec trading_loop config_file 
                     prev_price momentum inventory ema_ratio 
                     backup_price backup_momentum backup_ema =
  
  let cfg = load_config config_file in

  try
    let input_line_raw = input_line stdin in 

    match parse_line input_line_raw with
    
    | Some ("INFO", budget_raw, current_price, total_shares_real) ->
        
        let budget = budget_raw *. 0.95 in 

        let current_ratio =  
          if prev_price = 0.0 then 1.0
          else current_price /. prev_price 
        in
        
        let new_momentum = momentum *. current_ratio in
        let new_pct_acc = (new_momentum -. 1.0) *. 100.0 in
        let range_f = new_pct_acc /. cfg.min_margin in 
        let current_range = int_of_float range_f in

        let new_ema_ratio = 
            if ema_ratio = 0.0 then current_ratio 
            else (current_ratio *. cfg.alpha) +. (ema_ratio *. (1.0 -. cfg.alpha))
        in

        let predicted_momentum = momentum *. new_ema_ratio in
        let predicted_pct_acc = (predicted_momentum -. 1.0) *. 100.0 in
        let predicted_range_f = predicted_pct_acc /. cfg.min_margin in 
        let predicted_range = int_of_float predicted_range_f in

        (* --- DECISION LOGIC --- *)
        let (action_str, _, _) = 
          
          let qty_to_buy = 
            let max_qty = budget /. current_price in
            let max_ranges_diff = cfg.max_margin /. cfg.min_margin in 
            let diff_from_prediction = 
              if current_range < predicted_range then float_of_int (predicted_range - current_range) else 0.0
            in
            let qty_to_buy_raw = shares_amount max_ranges_diff diff_from_prediction max_qty cfg.buy_level in
            let q = if qty_to_buy_raw *. current_price > budget then budget /. current_price else qty_to_buy_raw in
            round3 (max 0.0 q)
          in
          
          let max_qty_to_sell = 
            let max_ranges_diff = cfg.max_margin /. cfg.min_margin in 
            let diff_from_prediction = 
              if current_range > predicted_range then float_of_int (current_range - predicted_range) else 0.0
            in
            let qty_to_sell_raw = shares_amount max_ranges_diff diff_from_prediction total_shares_real cfg.sell_level in
            round3 (if qty_to_sell_raw > total_shares_real then total_shares_real else qty_to_sell_raw)
          in

          let qty_to_sell_raw_calc = 
            PriceMap.fold (fun buy_range qty_held shares_sold ->
              if shares_sold < max_qty_to_sell && buy_range < current_range - 2 then shares_sold +. qty_held
              else shares_sold
            ) inventory 0.0
          in
 
          let qty_to_sell = round3 (min total_shares_real (max 0.0 qty_to_sell_raw_calc)) in
    
          (* --- MINIMUM NOTIONAL FILTER --- *)
          let min_val = cfg.min_notional in

          if qty_to_buy > qty_to_sell then
            let diff = qty_to_buy -. qty_to_sell in
            let val_usd = diff *. current_price in
            
            if val_usd < min_val then ("HOLD", inventory, new_momentum)
            else (Printf.sprintf "BUY %.4f" diff, inventory, new_momentum)

          else if qty_to_sell > qty_to_buy then
            let q_s = qty_to_sell -. qty_to_buy in
            let val_usd = q_s *. current_price in
            
            if val_usd < min_val then ("HOLD", inventory, new_momentum)
            else (Printf.sprintf "SELL %.4f" q_s, inventory, new_momentum)
          else 
            ("HOLD", inventory, new_momentum)
          in

        print_endline (action_str);
        flush stdout; 

        (* Recursion with backup state *)
        trading_loop config_file 
                     current_price new_momentum inventory new_ema_ratio 
                     prev_price momentum ema_ratio

    | Some ("ROLLBACK", _, _, _) ->
        trading_loop config_file 
                     backup_price backup_momentum inventory backup_ema 
                     backup_price backup_momentum backup_ema

    | Some ("BOUGHT", _, _, qty) ->
        let pct_acc = (momentum -. 1.0) *. 100.0 in
        let current_range = int_of_float (pct_acc /. cfg.min_margin) in
        let current_qty = try PriceMap.find current_range inventory with Not_found -> 0.0 in
        let new_inventory = PriceMap.add current_range (current_qty +. qty) inventory in
        trading_loop config_file 
                     prev_price momentum new_inventory ema_ratio 
                     backup_price backup_momentum backup_ema

    | Some ("SOLD", _, _, qty) ->
        let (remaining_to_remove, final_inventory) = 
             PriceMap.fold (fun r q (rem, m) ->
                if rem <= 0.0 then (0.0, PriceMap.add r q m)
                else if q > rem then (0.0, PriceMap.add r (q -. rem) m)
                else (rem -. q, m) 
             ) inventory (qty, PriceMap.empty)
        in
        trading_loop config_file 
                     prev_price momentum final_inventory ema_ratio 
                     backup_price backup_momentum backup_ema

    | _ -> 
        trading_loop config_file prev_price momentum inventory ema_ratio backup_price backup_momentum backup_ema

  with End_of_file -> () 

let () =
  trading_loop "params.txt" 
               0.0 1.0 PriceMap.empty 0.0 
               0.0 1.0 0.0