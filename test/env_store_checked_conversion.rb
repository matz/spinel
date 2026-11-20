# frozen_string_literal: true

class EnvStoreText < String
end

class EnvStoreArgument
  def initialize(result, moved_key = nil)
    @result = result
    @moved_key = moved_key
  end

  def to_str
    @moved_key << "_AFTER" if @moved_key
    GC.start
    @result
  end
end

key = +"SPINEL_ENV_STORE_CONVERSION"
text = EnvStoreText.new("value")
operand = EnvStoreArgument.new(text, key)
result = ENV.store(EnvStoreArgument.new(key), operand)
p [result.equal?(text), result.class, ENV[key]]

operand = EnvStoreArgument.new(text)
result = (ENV[key] = operand)
p [result.equal?(operand), ENV[key]]
text << "!"
p [text, ENV[key]]

begin
  ENV.store(key, EnvStoreArgument.new(nil))
rescue TypeError => error
  puts error.message
end

for rejected in [Encoding::UTF_8, String]
  begin
    ENV[key] = rejected
  rescue TypeError => error
    p [error.message, ENV[key]]
  end
end

begin
  ENV.store(Encoding::UTF_8, text)
rescue TypeError => error
  puts error.message
end

p ENV.store(key, nil)
