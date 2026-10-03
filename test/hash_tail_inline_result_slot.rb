# The inlined Enumerable body's Hash result has its own slot. Its key kind
# need not match the enclosing method's return Hash.
def string_groups
  { groups: ["alpha", "beta", "alpha"].group_by { |word| word }
      .transform_values { |words| words.length } }
end

def symbol_groups
  { "groups" => [:alpha, :beta, :alpha].group_by { |word| word }
      .transform_values { |words| words.length } }
end

def mixed_outer
  { "groups" => ["alpha", "beta", "alpha"].group_by { |word| word }
      .transform_values { |words| words.length }, status: "ready" }
end

# A begin result also has a destination distinct from the method's return.
def begin_groups
  groups = begin
    ["alpha", "beta", "alpha"].group_by { |word| word }
      .transform_values { |words| words.length }
  rescue RuntimeError
    { "failed" => 0 }
  end
  { groups: groups }
end

p string_groups[:groups].keys
p string_groups[:groups].values
p symbol_groups["groups"].keys
p symbol_groups["groups"].values
p mixed_outer.keys
p mixed_outer["groups"].keys
p mixed_outer["groups"].values
p mixed_outer[:status]
p begin_groups[:groups].keys
p begin_groups[:groups].values
