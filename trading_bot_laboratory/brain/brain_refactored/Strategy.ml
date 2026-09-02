let shares_amount max_r current_r max_shares level =
  if max_r = 0.0 then 0.0 else
    let x = current_r /. max_r in
    let x_clamped = max 0.0 (min 1.0 x) in
    let y = 1.0 -. (1.0 -. x_clamped ** level) ** (1.0 /. level) in
    max_shares *. y

let round3 f = (floor (f *. 1000.0 +. 0.5)) /. 1000.0
