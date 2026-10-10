# spinel: int64 -- -2^63 is an Integer past 2^31; not run on a 32-bit target
# Array.new(64) holds nil until a slot is computed; the slot computed to
# -2**63 is a number afterwards and the other slots are still nil.
n = ARGV.size
sign = ~(0x7fffffffffffffff - n)
tbl = Array.new(64)
tbl[63] = sign
p tbl[63]
p tbl[63].nil?
p tbl[0].nil?
p tbl[64].nil?
p(tbl[63] ? 1 : 0)
p(tbl[0] ? 1 : 0)
p tbl[63] & (-1 - n)
tbl[63] ||= 5
p tbl[63] == 5
tbl[0] ||= 5
p tbl[0]
p tbl.compact.size
p tbl.compact
p tbl.count(&:nil?)
p tbl.count(nil)
p tbl.index(nil)
p tbl.rindex(nil)
p tbl.include?(nil)
p tbl.include?(sign)
p tbl.index(sign)
p tbl.last
p tbl.first(2)
p tbl.last(2)
p tbl[62, 2]
p tbl[62..]
p tbl.each_with_index.select { |e, _| e }.map(&:last)
p tbl.map { |e| e.nil? ? "-" : "v" }.join
p tbl.sum { |e| e || 0 }
p tbl.compact.sum
p tbl.compact.min
p tbl.compact.inspect
p tbl.values_at(0, 63)
p tbl.reverse.first
tbl.delete_at(0)
p tbl.size, tbl.last, tbl.compact.size
tbl.insert(0, nil)
p tbl.size, tbl.last
tbl.unshift(sign)
p tbl.first, tbl.last, tbl.compact.size
p tbl.compact.uniq.size
