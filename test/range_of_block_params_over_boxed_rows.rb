# A Range of two block parameters that each yields from an Array of
# Arrays: under --int-overflow=promote the rows hold boxed values, and a
# parameter is one of them, so the Range takes its kind from the values
# (a String Range here, an Integer one for numbers) as in the other modes.
rows = [["1", "20"], ["9", "11"], ["a", "c"]]
rows.each do |b, e|
  [false, true].each do |ex|
    r = ex ? (b...e) : (b..e)
    p [r, r.include?("10"), r.include?("b"), r.to_a.size]
  end
end
nums = [[1, 4], [10, 12]]
nums.each { |lo, hi| r = (lo..hi); p [r, r.sum, r.include?(11)] }
