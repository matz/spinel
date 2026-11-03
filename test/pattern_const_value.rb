# A constant that holds a value, in a pattern: value === subject.
LIMIT = 3
NAME = "a"
TAG = :t
RATE = 1.5
p [3, 4, 3.0, "a", :t, 1.5, nil].map { |v|
  case v
  in LIMIT then :limit
  in NAME then :name
  in TAG then :tag
  in RATE then :rate
  else :no
  end
}
p(case 3; in LIMIT then :limit; else :no; end)
p(case 4; in LIMIT then :limit; else :no; end)
p(case "a"; in NAME then :name; else :no; end)
p(case "b"; in NAME then :name; else :no; end)
p(case 3; in NAME then :name; else :no; end)
p(case 1.5; in RATE then :rate; else :no; end)
p(case :t; in TAG | LIMIT then :either; else :no; end)
p(case [3, "a"]; in [LIMIT, NAME] then :pair; else :no; end)

# an Integer or Float subject that holds nil equals no value
ZERO = 0
FZERO = 0.0
def pick(i) = i.even? ? i : nil
def pick_f(i) = i.even? ? 0.0 : nil
v = pick(1); f = pick_f(1); w = pick(0)
p(case v; in ZERO then :z; else :no; end)
p(case f; in FZERO then :z; else :no; end)
p(case v; in 0 then :z; else :no; end)
p(case f; in 0.0 then :z; else :no; end)
p(case w; in ZERO then :z; else :no; end)
p(case v; in FZERO then :z; else :no; end)

# inside a typed Array or Hash pattern
p(case [3]; in [LIMIT] then :a; else :no; end)
p(case [3, 4]; in [LIMIT, Integer] then :a; else :no; end)
p(case [4]; in [LIMIT] then :a; else :no; end)
p(case({a: 3}); in {a: LIMIT} then :h; else :no; end)
p(case({a: 4}); in {a: LIMIT} then :h; else :no; end)
p(case ["a"]; in [NAME] then :s; else :no; end)
