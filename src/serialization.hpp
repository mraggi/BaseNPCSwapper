#pragma once
#include <F4SE/F4SE.h>
#include <cstdint>

namespace BNS::Serialization
{
constexpr uint32_t BNSF4_SERIALIZATION_ID = 'BNS4';
constexpr uint32_t BNSF4_RECORD_ACTR = 'BNSA';

void SaveCallback(const F4SE::SerializationInterface* a_intfc);
void LoadCallback(const F4SE::SerializationInterface* a_intfc);
void RevertCallback(const F4SE::SerializationInterface* a_intfc);
} // namespace BNS::Serialization
