module Boom; end
$tries << 1
$tries.then { |t| raise "first" if t.size < 2 }
$tries << :ok
