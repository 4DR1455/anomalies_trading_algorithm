open Types

let rec trading_loop config_file prev_price momentum ema_ratio cooldown inventory cycles_horiz b_p b_m b_e b_cy mode =
  let cfg = Config.load_config config_file in
  
  match Comm.read_message () with
  | Comm.EOF -> ()
  | Comm.Info (budget, current_price, total_shares) ->
      let current_ratio = if prev_price = 0.0 then 1.0 else current_price /. prev_price in
      let new_momentum = momentum *. current_ratio in
      let new_ema_ratio = if ema_ratio = 0.0 then momentum else (momentum *. cfg.alpha) +. (ema_ratio *. (1.0 -. cfg.alpha)) in
      let diff = (new_momentum /. new_ema_ratio) -. 1.0 in
      let dist_anomaly = abs_float (diff *. 100.0) in
      let gmr = new_ema_ratio -. ema_ratio in

      let (next_mode, transition_happened) = 
        if mode = 0 then
          if gmr <= cfg.freeze_zone then (Comm.send_cmd "FREEZE"; (1, true))
          else (0, false)
        else 
          if gmr > cfg.quit_zone then 
            (Comm.send_cmd "UNFREEZE"; (0, true))
          else (1, false)
      in

      if transition_happened then
         trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 current_price new_momentum new_ema_ratio cycles_horiz next_mode
      
      else if cooldown > 0 then begin
        Comm.send_hold ();
        trading_loop config_file current_price new_momentum new_ema_ratio (cooldown - 1) inventory (cycles_horiz + 1) b_p b_m b_e b_cy mode
      end 
      
      else if dist_anomaly > cfg.max_anomaly then begin
        Comm.send_hold ();
        trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 current_price new_momentum new_ema_ratio cycles_horiz mode
      end 
      
      else if mode = 0 then begin
        let is_grid_occupied = false in 
        
        if diff < 0.0 && dist_anomaly > cfg.p_margin then
          let max_qty = budget /. current_price in
          let final_q = Strategy.round3 (Strategy.shares_amount cfg.max_anomaly dist_anomaly max_qty cfg.k) in
          
          if final_q *. current_price > cfg.min_notional && dist_anomaly < cfg.max_anomaly then begin
            let entry_r = new_momentum *. 1.0001 in
            let exit_r = new_ema_ratio *. 0.9999 in
            Comm.send_vall final_q entry_r exit_r;
            trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz mode
          end else begin
            Comm.send_hold ();
            trading_loop config_file current_price new_momentum new_ema_ratio cooldown inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode
          end
        
        else if diff > 0.0 && dist_anomaly > cfg.p_margin then
          let final_q = Strategy.round3 (Strategy.shares_amount cfg.max_anomaly dist_anomaly total_shares cfg.k) in
          
          if final_q *. current_price > cfg.min_notional && dist_anomaly < cfg.max_anomaly then begin
            let entry_r = new_momentum *. 0.9999 in
            let exit_r = new_ema_ratio *. 1.0001 in
            Comm.send_pic final_q entry_r exit_r;
            trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz mode
          end else begin
            Comm.send_hold ();
            trading_loop config_file current_price new_momentum new_ema_ratio cooldown inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode
          end
           
        else if cycles_horiz >= 60 && (not is_grid_occupied) && budget > (cfg.min_notional *. 1.5) then begin
          let qty = Strategy.round3 ((budget *. 0.25) /. current_price) in
          let entry_r = new_momentum *. 1.0001 in
          let exit_r = new_momentum *. (1.0 +. (cfg.p_margin /. 100.0)) in
          Comm.send_vall qty entry_r exit_r;
          trading_loop config_file current_price new_momentum new_ema_ratio 45 inventory 0 prev_price momentum ema_ratio cycles_horiz mode
        end else begin
          Comm.send_hold ();
          trading_loop config_file current_price new_momentum new_ema_ratio 0 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode
        end
      end 
      
      else begin
        Comm.send_hold ();
        trading_loop config_file current_price new_momentum new_ema_ratio 0 inventory (cycles_horiz + 1) current_price new_momentum new_ema_ratio (cycles_horiz + 1) mode
      end

  | Comm.Bought id ->
      let new_inv = Types.GridMap.add id true inventory in
      trading_loop config_file prev_price momentum ema_ratio 45 new_inv 0 b_p b_m b_e b_cy mode

  | Comm.Sold id ->
      let new_inv = Types.GridMap.add id false inventory in
      trading_loop config_file prev_price momentum ema_ratio 0 new_inv cycles_horiz b_p b_m b_e b_cy mode

  | Comm.Rollback ->
      trading_loop config_file b_p b_m b_e 0 inventory b_cy b_p b_m b_e b_cy mode

  | Comm.Unknown ->
      trading_loop config_file prev_price momentum ema_ratio cooldown inventory (cycles_horiz + 1) b_p b_m b_e cycles_horiz mode
