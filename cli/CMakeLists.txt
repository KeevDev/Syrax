# El CLI no enlaza contra syrax: solo genera proyectos e invoca a cmake.
# Mantenerlo independiente lo hace compilar en segundos.
add_executable(syrax_cli main.cpp)
set_target_properties(syrax_cli PROPERTIES OUTPUT_NAME syrax)
target_compile_features(syrax_cli PRIVATE cxx_std_23)
target_compile_definitions(syrax_cli PRIVATE SYRAX_VERSION="${PROJECT_VERSION}")

install(TARGETS syrax_cli RUNTIME DESTINATION bin)
