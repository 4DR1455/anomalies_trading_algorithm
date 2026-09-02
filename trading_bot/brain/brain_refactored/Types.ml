module FloatKey = struct
  type t = float
  let compare = Float.compare
end

module GridMap = Map.Make(FloatKey)

type config = {
  p_margin     : float;
  max_anomaly  : float;
  k            : float;
  alpha        : float;
  min_notional : float;
}