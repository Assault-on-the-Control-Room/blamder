#pragma once

#include <string>
#include <iostream>

#include "BLI_small_vector.hpp"
#include "BLI_small_map.hpp"
#include "BLI_shared.hpp"

namespace FN {

	using namespace BLI;

	class Type;
	class Signature;
	class Function;

	using SharedType = Shared<const Type>;
	using SharedFunction = Shared<Function>;
	using SmallTypeVector = SmallVector<SharedType>;

	class Composition {
	public:
		typedef void (*FreeFunction)(void *value);

	private:
		struct Entry {
			void *value;
			FreeFunction free;
		};

	public:
		template<typename T>
		void add(T *value)
		{
			this->m_elements.add(this->get_key<T>(), this->create_entry(value));
		}

		template<typename T>
		inline T *get() const
		{
			uint64_t key = this->get_key<T>();
			if (this->m_elements.contains(key)) {
				return (T *)this->m_elements.lookup(key).value;
			}
			else {
				return nullptr;
			}
		}

		~Composition()
		{
			for (const Entry &entry : this->m_elements.values()) {
				entry.free(entry.value);
			}
		}

	private:
		template<typename T>
		static uint64_t get_key()
		{
			return (uint64_t)T::identifier;
		}

		template<typename T>
		static FreeFunction get_free_function()
		{
			return T::free;
		}

		template<typename T>
		Entry create_entry(T *value)
		{
			Entry entry;
			entry.value = (void *)value;
			entry.free = this->get_free_function<T>();
			return entry;
		}

		BLI::SmallMap<uint64_t, Entry> m_elements;
	};

	class Type {
	public:
		const std::string &name() const
		{
			return this->m_name;
		}

		template<typename T>
		inline const T *extension() const
		{
			return this->m_extensions.get<T>();
		}

		template<typename T>
		void extend(const T *extension)
		{
			BLI_assert(this->m_extensions.get<T>() == nullptr);
			this->m_extensions.add(extension);
		}

	protected:
		std::string m_name;

	private:
		Composition m_extensions;
	};

	class Parameter {
	public:
		Parameter(const std::string &name, SharedType &type)
			: m_type(type), m_name(name) {}

		const SharedType &type() const
		{
			return this->m_type;
		}

		const std::string &name() const
		{
			return this->m_name;
		}

	private:
		SharedType m_type;
		const std::string m_name;
	};

	class InputParameter : public Parameter {
	public:
		InputParameter(const std::string &name, SharedType &type)
			: Parameter(name, type) {}
	};

	class OutputParameter : public Parameter {
	public:
		OutputParameter(const std::string &name, SharedType &type)
			: Parameter(name, type) {}
	};

	using InputParameters = SmallVector<InputParameter>;
	using OutputParameters = SmallVector<OutputParameter>;

	class Signature {
	public:
		Signature() = default;
		~Signature() = default;

		Signature(const InputParameters &inputs, const OutputParameters &outputs)
			: m_inputs(inputs), m_outputs(outputs) {}

		inline const InputParameters &inputs() const
		{
			return this->m_inputs;
		}

		inline const OutputParameters &outputs() const
		{
			return this->m_outputs;
		}

		SmallTypeVector input_types() const
		{
			SmallTypeVector types;
			for (const InputParameter &param : this->inputs()) {
				types.append(param.type());
			}
			return types;
		}

		SmallTypeVector output_types() const
		{
			SmallTypeVector types;
			for (const OutputParameter &param : this->outputs()) {
				types.append(param.type());
			}
			return types;
		}

	private:
		const InputParameters m_inputs;
		const OutputParameters m_outputs;
	};

	class Function {
	public:
		Function(const Signature &signature, const std::string &name = "Function")
			: m_signature(signature), m_name(name) {}

		virtual ~Function() {}

		inline const Signature &signature() const
		{
			return this->m_signature;
		}

		template<typename T>
		inline const T *body() const
		{
			return this->m_bodies.get<T>();
		}

		template<typename T>
		void add_body(const T *body)
		{
			BLI_assert(this->m_bodies.get<T>() == nullptr);
			this->m_bodies.add(body);
		}

		const std::string &name() const
		{
			return this->m_name;
		}

	private:
		const Signature m_signature;
		Composition m_bodies;
		const std::string m_name;
	};

} /* namespace FN */