# A Hash reaching a call a user class also answers, given a keyword whose
# value is computed (`merge!(k: f(x))`, Warden's call_failure_app): the
# builtin arm reads the value the dispatch evaluated, once.
class Opts
  def merge!(h)
    puts "Opts#merge! #{h[:path]}"
    self
  end
end

$runs = 0
def path_of(x)
  $runs += 1
  "/p" + x.to_s
end

def note(o, x)
  o.merge!(path: path_of(x))
  o
end

p note({a: 1}, 1)
note(Opts.new, 2)
p $runs

def note_two(o, x)
  o.merge!(path: path_of(x), at: x * 10)
end
p note_two({}, 3)
p $runs
