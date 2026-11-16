# ENV boxed: held where other values are held too (a method that answers
# ENV or a Hash, a Hash's value, an Array's element) it is still ENV. A
# call on it reads the environment at the call, a store, a delete or an
# update through it changes the environment, and it is equal? to ENV.
def pick(f) = f ? ENV : { "SPX_Z" => "h" }
v = pick(ARGV.empty?)
w = pick(!ARGV.empty?)
v["SPX_A"] = "1"
p ENV["SPX_A"]
ENV["SPX_B"] = "2"
p v["SPX_B"]
p v.equal?(ENV), w.equal?(ENV)
p v.class, w.class
p v.to_s
p v.key?("SPX_B"), v.fetch("SPX_NO", "d")
v.store("SPX_C", "3")
p ENV["SPX_C"]
p v.delete("SPX_C"), ENV.key?("SPX_C")
p v.select { |k, _| k.start_with?("SPX_") }.keys.sort
p w["SPX_Z"], w.size
params = { "m" => "GET" }
params["e"] = ENV
params["e"]["SPX_D"] = "4"
p ENV["SPX_D"]
ENV["SPX_E"] = "5"
p params["e"]["SPX_E"]
p [ENV].first["SPX_A"]
p({ "e" => ENV }["e"].select { |k, _| k == "SPX_A" })
v.update("SPX_F" => "6")
p ENV["SPX_F"]
ENV["SPX_M1"] = "a"; ENV["SPX_M2"] = "b"
v.reject! { |k, _| k == "SPX_M1" }
v.select! { |k, _| k != "SPX_M2" }
p ENV.key?("SPX_M1"), ENV.key?("SPX_M2")
s = v.shift
p s.class, ENV.key?(s[0])
