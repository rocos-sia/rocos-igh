find_path(EtherCAT_INCLUDE_DIR
    NAMES ecrt.h
    HINTS ENV ETHERCAT_ROOT
    PATH_SUFFIXES include
)
find_library(EtherCAT_LIBRARY
    NAMES ethercat
    HINTS ENV ETHERCAT_ROOT
    PATH_SUFFIXES lib lib64
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EtherCAT
    REQUIRED_VARS EtherCAT_INCLUDE_DIR EtherCAT_LIBRARY
)

if(EtherCAT_FOUND AND NOT TARGET EtherCAT::EtherCAT)
    add_library(EtherCAT::EtherCAT UNKNOWN IMPORTED)
    set_target_properties(EtherCAT::EtherCAT PROPERTIES
        IMPORTED_LOCATION "${EtherCAT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${EtherCAT_INCLUDE_DIR}"
    )
endif()

mark_as_advanced(EtherCAT_INCLUDE_DIR EtherCAT_LIBRARY)
