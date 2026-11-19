# A StrPolyHash must keep the Proc stored in dproc_self alive. The hash is the
# only owner after the local Proc is cleared; a forced collection must not turn
# a later default lookup into a call through freed memory.
suffix = "!"
h = { "seed" => 1, "text" => "v" }
h.default_proc = proc { |hh, key| key + suffix }
GC.start
p h["missing"]

def fresh_hash
  hash = {"seed" => 1, "text" => "v"}
  hash.default_proc = proc { |owner, key| [owner["seed"], key] }
  hash
end

def default_key
  GC.start
  +"key"
end

p fresh_hash.default(default_key)
p fresh_hash.default(12)
