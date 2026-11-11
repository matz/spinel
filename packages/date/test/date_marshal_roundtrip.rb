# A Date through Marshal answers the same day. Date#marshal_load
# calls initialize(jd, sg, nil, nil, nil, df), which no DateTime runs (its
# marshal_load is its own); dispatched to DateTime's initialize, the nil was
# refused as its Integer day (#11613).
require "date"

d = Date.new(2024, 1, 2)
e = Marshal.load(Marshal.dump(d))
puts e
puts e == d
puts Date.new(2024, 1, 2).dup.marshal_load([0, 2460000, 0, 0, 0, 2299161.0])
