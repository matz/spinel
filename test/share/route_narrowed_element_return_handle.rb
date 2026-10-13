# A method answering an element it narrowed to a String, or its parameter
# (`v.is_a?(String) ? v : fallback`), hands on the String itself where the
# rule shares it: written to a variable, passed as an argument or kept as a
# block's value, the value is that String, not a copy (#8328, a Rails
# application's `Params.str(params, "signed_id", "")`).
module Params
  def self.str(sub, key, fallback)
    value = sub.fetch(key, fallback)
    value.is_a?(String) ? value : fallback
  end
end

def find_signed(id) = "blob:" + id

def same(a, b) = a.equal?(b)

signed = +"abc"
signed << "def"
none = +"none"
params = { "signed_id" => signed, "n" => 1 }
p find_signed(Params.str(params, "signed_id", ""))
p find_signed(Params.str(params, "n", none))
id = Params.str(params, "signed_id", "")
p id.equal?(signed)
p same(Params.str(params, "n", none), none)
p [1].map { Params.str(params, "signed_id", "") }[0].equal?(signed)
