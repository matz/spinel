require "openssl"
point = OpenSSL::PKey::EC::Point.new(OpenSSL::PKey::EC::Group.new("prime256v1"))
[:bogus, "compressed", nil, 4].each do |form|
  begin
    point.to_octet_string(form)
    puts "accepted"
  rescue ArgumentError => error
    puts error.class
    puts error.message
  end
end
