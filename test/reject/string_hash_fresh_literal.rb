# A fresh String stored in a Hash must not silently lose its append.
{k: +"h"}.each_value { |x| x << "!"; p x }
