# A sort, min or max block that answers nil says the two elements do not
# compare: CRuby raises ArgumentError ("comparison of A with b failed").
# The boxed answer was read with sp_poly_to_i, so nil was 0 -- "equal" --
# and the sort kept its order, max answered the first element.

def t(s)
  r = yield
  puts "#{s}: #{r.inspect}"
rescue ArgumentError => e
  puts "#{s}: #{e.class}"
end

src = [1, nil, "s"]
t("sort") { [3, 1, 2].sort { |a, b| src[1] } }
t("sort!") { [3, 1, 2].sort! { |a, b| src[1] } }
t("max") { [3, 1, 2].max { |a, b| src[1] } }
t("min") { [3, 1, 2].min { |a, b| src[1] } }
t("strs") { %w[b a].sort { |a, b| src[1] } }
t("ok sort") { [3, 1, 2].sort { |a, b| a <=> b } }
t("ok poly") { [3, 1, 2].sort { |a, b| [b <=> a, "x"][0] } }
t("ok max") { [3, 1, 2].max { |a, b| [a <=> b, "x"][0] } }
