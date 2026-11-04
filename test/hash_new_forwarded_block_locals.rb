# A default block an inlined method forwards to Hash.new (`def mk(&b) =
# Hash.new(&b)`, tzinfo's StringDeduper#create_hash) with a local of its
# own: the bare default function declared only the hash and the key, and
# the C compile failed on the local (`use of undeclared identifier`).
def mk(&b)
  Hash.new(&b)
end

doubled = mk { |h, k| v = k * 2; h[k] = v }
p doubled[3], doubled

deduped = mk { |h, k| v = k.dup.freeze; h[v] = v }
s = deduped["ab"]
p s, s.frozen?, deduped["ab"].equal?(s)

counted = mk do |h, k|
  n = 0
  k.each_char { |ch| n += 1 if ch == "a" }
  h[k] = n
end
p counted["banana"]
