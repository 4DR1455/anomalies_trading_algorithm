(**
 * ============================================================================
 * Strategy Brain Core (OCaml) - Single-Directional Mean Reversion Engine
 * ============================================================================
 * This module implements the deterministic core logic for the production 
 * trading execution. Unlike the dual-directional laboratory branch, this 
 * engine is optimized for 100% capital efficiency by strictly targeting 
 * undervalued market anomalies (Valleys).
 * 
 * ALPHA: Small downwards maket anomalies/deviations.
 * 
 * Key Architectural Features:
 * 1. Asymmetric Capital Deployment: Maximizes ROI using downward market 
 *    anomalies by buying on price deviations and selling on price recovery.
 * 2. Grid-Based Position Tracking: Utilizes a virtual grid mapping system 
 *    (GridMap) to prevent over-exposure and redundant purchases within the 
 *    same price zone.
 * 3. Dynamic Position Sizing: Calculates optimal order quantities using 
 *    an asymptotic formula (SharesAmount) based on anomaly severity.
 * 4. Forced Liquidity Injection: Automatically executes a minor market 
 *    entry if no structural anomalies occur within a 60-cycle horizon, 
 *    ensuring continuous market participation.
 * 5. State Rollback: Preserves state integrity using backup variables 
 *    to seamlessly recover from API timeouts or missed chase executions.
 * ============================================================================
 *)

open Types

let rec trading_loop config_file prev_price momentum ema_ratio cooldown inventory cycles_horiz b_p b_m b_e b_cy =
  let cfg = Config.load_config config_file in
  let grid_size = cfg.p_margin *. 2.0 in

  match Comm.read_message () with
  | Comm.EOF -> ()
  | Comm.Info (budget, current_price, total_shares) ->
      let current_ratio = if prev_price = 0.0 then 1.0 else current_price /. prev_price in
      let new_momentum = momentum *. current_ratio in
      let new_ema_ratio = if ema_ratio = 0.0 then momentum else (momentum *. cfg.alpha) +. (ema_ratio *. (1.0 -. cfg.alpha)) in
      
      let current_grid_id = new_momentum /. grid_size in
      let is_grid_occupied = try GridMap.find current_grid_id inventory with Not_found -> false in

      let diff = (current_ratio /. new_ema_ratio) -. 1.0 in
      let dist_anomaly = abs_float diff in

      if cooldown > 0 then begin
        Comm.send_hold ();
        trading_loop config_file current_price new_momentum new_ema_ratio (cooldown - 1) inventory (cycles_horiz + 1) b_p b_m b_e b_cy
      end
      else if dist_anomaly > cfg.max_anomaly then begin
        Comm.send_hold ();
        (* Apliquem cooldown de 45 cicles però MANTENIM la continuïtat de l'evolució i el cicle *)
        trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1)
      end
      else if diff < 0.0 && dist_anomaly > cfg.p_margin then begin
        let max_qty = budget /. current_price in
        let final_q = Strategy.round3 (Strategy.shares_amount cfg.max_anomaly dist_anomaly max_qty cfg.k) in
        
        if final_q *. current_price > cfg.min_notional && dist_anomaly < cfg.max_anomaly then begin
          Comm.send_buy final_q new_ema_ratio;
          trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz
        end else begin
          Comm.send_hold ();
          trading_loop config_file current_price new_momentum new_ema_ratio cooldown inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1)
        end
      end
      else if cycles_horiz >= 60 && (not is_grid_occupied) && budget > (cfg.min_notional *. 1.5) then begin
        let qty = Strategy.round3 ((budget *. 0.025) /. current_price) in
        Comm.send_buy qty (new_momentum +. (cfg.p_margin));
        trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz
      end
      else begin
        Comm.send_hold ();
        trading_loop config_file current_price new_momentum new_ema_ratio 0 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1)
      end

  | Comm.Bought id ->
      let new_inv = Types.GridMap.add id true inventory in
      trading_loop config_file prev_price momentum ema_ratio 45 new_inv 0 b_p b_m b_e b_cy

  | Comm.Sold id ->
      let new_inv = Types.GridMap.add id false inventory in
      trading_loop config_file prev_price momentum ema_ratio 0 new_inv cycles_horiz b_p b_m b_e b_cy

  | Comm.Rollback ->
      trading_loop config_file b_p b_m b_e 0 inventory b_cy b_p b_m b_e b_cy

  | Comm.Unknown ->
      trading_loop config_file prev_price momentum ema_ratio cooldown inventory (cycles_horiz + 1) b_p b_m b_e cycles_horiz
