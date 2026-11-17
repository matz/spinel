# Definite-length, universal-tag encoding only. libcrypto owns DER and OID
# interpretation; decoding, tagging and the remaining ASN.1 types are absent.
module OpenSSL
  module Native
    native_lib "openssl"
    native_obj "packages/openssl/sp_openssl.o"
    native_func :asn1_der, [:int, :string, :int], :cbinstr, "sp_openssl_asn1_der"
    native_func :asn1_integer, [:any], :cbinstr, "sp_openssl_asn1_integer"
    native_func :asn1_unused_bits, [:any], :any, "sp_openssl_asn1_unused_bits"
    native_func :asn1_name, [:string, :int], :cbinstr, "sp_openssl_asn1_name"
  end

  module ASN1
    class ASN1Error < OpenSSLError; end

    class ASN1Data
      attr_reader :value, :tag, :tag_class
      def initialize(value, tag, tag_class)
        raise ASN1Error, "invalid tag class" unless tag_class.is_a?(Symbol)
        @value, @tag, @tag_class = value, tag, tag_class
        self
      end
    end
    class Primitive < ASN1Data; end
    class Constructive < ASN1Data
      include Enumerable

      def each
        if block_given?
          @value.__send__(:each) { |*nodes| yield(*nodes) }
        else
          @value.__send__(:each)
        end
        self
      end
    end

    class Sequence < Constructive
      def initialize(value, *tagging)
        raise NoMethodError, "ASN.1 tagging is unavailable" unless tagging.empty?
        super(value, 16, :UNIVERSAL)
      end
      def to_der
        raise ASN1Error, "invalid tag" if @tag.nil?
        values = @value
        values = values.__send__(:to_a) if !values.is_a?(Array) && values.respond_to?(:to_a, true)
        raise TypeError, "expected Array" unless values.is_a?(Array)
        data = "".b
        values.each do |child|
          child = child.to_der if child.respond_to?(:to_der)
          data << OpenSSL.send(:comparison_string, child)
        end
        Native.asn1_der(16, data, 0)
      end
    end

    class ObjectId < Primitive
      def initialize(value, *tagging)
        raise NoMethodError, "ASN.1 tagging is unavailable" unless tagging.empty?
        super(value, 6, :UNIVERSAL)
      end
      def to_der
        Native.asn1_der(6, OpenSSL.send(:comparison_string, @value), 0)
      end
      def oid
        Native.asn1_name(OpenSSL.send(:comparison_string, @value), 0).force_encoding(Encoding::US_ASCII)
      end
      def sn
        name = Native.asn1_name(OpenSSL.send(:comparison_string, @value), 1)
        name.empty? ? nil : name
      end
      def ln
        name = Native.asn1_name(OpenSSL.send(:comparison_string, @value), 2)
        name.empty? ? nil : name
      end
      # Object identity would silently disagree with CRuby's OID equality.
      def ==(other)
        raise NoMethodError, "OpenSSL::ASN1::ObjectId#== is unavailable"
      end
    end

    class BitString < Primitive
      attr_accessor :unused_bits
      def initialize(value, *tagging)
        raise NoMethodError, "ASN.1 tagging is unavailable" unless tagging.empty?
        super(value, 3, :UNIVERSAL)
        @unused_bits = 0
        self
      end
      def to_der
        bits = Native.asn1_unused_bits(@unused_bits)
        if bits.nil?
          input = @unused_bits
          raise TypeError, "no implicit conversion into Integer" unless input.respond_to?(:to_int, true)
          converted = input.__send__(:to_int)
          raise TypeError, "no implicit conversion into Integer" unless converted.is_a?(::Integer)
          bits = Native.asn1_unused_bits(converted)
        end
        raise ASN1Error, "unused_bits for a bitstring value must be in the range 0 to 7" if bits < 0 || bits > 7
        Native.asn1_der(3, OpenSSL.send(:comparison_string, @value), bits)
      end
    end

    class Integer < Primitive
      def initialize(value, *tagging)
        raise NoMethodError, "ASN.1 tagging is unavailable" unless tagging.empty?
        super(value, 2, :UNIVERSAL)
      end
      def to_der
        number = @value
        number = number.instance_variable_get(:@value) || 0 if number.is_a?(BN)
        raise TypeError, "Cannot convert into OpenSSL::BN" unless number.is_a?(::Integer)
        Native.asn1_integer(number)
      end
    end
  end
end
