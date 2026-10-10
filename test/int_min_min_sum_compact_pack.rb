# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# -2**63 through Array#min / max / sum / compact and a q pack-unpack round
# trip, as a run-time value in every slot.
n = ARGV.size
m = -9223372036854775807 - (n + 1)
p [n, m].min
p [m, n].min
p [m, 1].max
p [m].max
p [m].min
p [-9223372036854775807, -(n + 1)].sum
p [m, 0].sum
p [m].sum
p [1, 2, m].sum - m
p [m, nil].compact
p [nil, m, nil].compact
p [nil, m, nil].compact.size
p [m, nil].compact.first
p [m].pack("q<").bytes
p [m].pack("q<").unpack1("q<")
p [m].pack("q>").unpack1("q>")
p [m].pack("Q<").unpack1("q<")
p [m].pack("q<").unpack("q<")
p [m].pack("q<").unpack1("q<").nil?
p [m, m].pack("q<q<").unpack("q<q<")
p [m].pack("q<").unpack1("Q<")
p "\x00\x00\x00\x00\x00\x00\x00\x80".unpack1("q<")
p "\x80\x00\x00\x00\x00\x00\x00\x00".unpack1("q>")
p [m].min_by { |e| e }
p [m, 0].minmax
p [m, 0].sort
p [0, m].sort
p [m, 0].sort { |a, b| b <=> a }
p [m, 0].max(2)
p [m, 0].min(1)
