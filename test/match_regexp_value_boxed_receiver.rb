# =~ with a Regexp value on a boxed receiver answers as CRuby: a Symbol by
# its name, nil with nil, another class NoMethodError naming it.
r = Regexp.new("b")
[nil, :abc, "xb", 5].each do |v|
  x = ARGV.empty? ? v : "zz"
  begin
    p x =~ r
  rescue NoMethodError => e
    p e.message
  end
end
