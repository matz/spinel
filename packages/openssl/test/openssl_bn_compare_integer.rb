require 'openssl'
['-18446744073709551617', '0', '7', '18446744073709551617'].each do |decimal|
  number=OpenSSL::BN.new(decimal).freeze
  [-18446744073709551618, 0, 7, 18446744073709551618].each do |integer|
    p number <=> integer
    p number <=> OpenSSL::BN.new(integer)
    p number.send(:<=>, integer)
    p number.__send__(:<=>, OpenSSL::BN.new(integer))
  end
end
[nil, '7', 7.0, Object.new].each do |other|
  begin
    OpenSSL::BN.new('7') <=> other
  rescue TypeError => error
    puts error.class
  end
end
