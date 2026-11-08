module Example
  #: (Hash[String, String]) -> Integer
  def self.size(options) = options.length
  #: () -> Hash[String, String]
  def self.empty = {}
end

p Example.size(Example.empty)
