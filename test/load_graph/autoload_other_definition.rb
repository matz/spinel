if ENV["SPINEL_REVIEW_NOPE"]
  autoload :Cnd, File.join(__dir__, "autoload_other_definition_lib")
else
  require_relative "autoload_other_definition_lib"
end
p Cnd.v
