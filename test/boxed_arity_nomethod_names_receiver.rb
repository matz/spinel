# A boxed value with no arity raises the NoMethodError CRuby raises,
# naming its class, where the Proc and the Method beside it answer theirs.
def two(a, b) = a

[->(a) { a }, method(:two), 3, "s", nil].each do |x|
  begin
    p x.arity
  rescue NoMethodError => e
    puts e.message
  end
end
