# Keep a live set of Strings while churning short-lived Arrays. The object
# collector visits the live set and reclaims the temporary Arrays.

held = []
20000.times { |i| held << ("held-" * 40 + i.to_s) }

total = 0
4000.times do |k|
  a = []
  40.times { |i| a << [i, i + 1] }
  total += a.length
end
puts "#{total} #{held.length}"
