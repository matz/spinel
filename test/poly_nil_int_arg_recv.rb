# A boxed nil read as an Integer argument or receiver: CRuby raises -- the
# conversion TypeError for an argument, NoMethodError for a receiver -- and
# Time's constructors default a nil field. These were read as 0: `~x` was -1,
# `x.even?` true, `xs[x]` the first element, `exit(x)` a successful exit,
# and Time.new(2020, x) an ArgumentError for month 0.

def t(s)
  r = yield
  puts "#{s}: #{r.inspect}"
rescue StandardError => e
  puts "#{s}: #{e.class}"
end

src = [1, nil, "s"]
x = src[1]
st = ["abcb", 1][0]

t("index") { src[x] }
t("~") { ~x }
t("even?") { x.even? }
t("odd?") { x.odd? }
t("chr") { x.chr(Encoding::UTF_8) }
t("str.index") { st.index("b", x) }
t("Time.new") { Time.new(2020, x, x, x).month }
t("Time.utc") { Time.utc(2020, x).day }
t("Time.local") { Time.local(2020, 2, x).day }
t("Time.utc year") { Time.utc(x) }
t("ok") { src[src[0]] }
begin
  exit(x)
rescue TypeError => e
  puts "exit: #{e.class}"
end
