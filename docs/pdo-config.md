
# 插值周期配置

通过60C2/2可以配置插值周期。可通过SDO写入该对象来修改插值周期。

# PDO Configuration

## RxPDO(Outputs)

| Object(Hex) / Hi Sub(Dec) | Name | Data Type | Attribute |
| :--- | :--- | :--- | :---  |
| 607A/0 | Target Position | INT32 | RW |
| 60FF/0 | Target Velocity | INT32 | RW  |
| 6071/0 | Target Torque | INT16 | RW |
| 6040/0 | Control word | UINT16 | RW  |
| 6060/0 | Modes of Operation | INT8 | RW |


## TxPDO (Inputs)

| Object(Hex) / Hi Sub(Dec) | Name | Data Type | Attribute |
| :--- | :--- | :--- | :--- |
| 6041/0 | Status word | UINT16 | RO  |
| 6064/0 | Position actual value | INT32 | RO  |
| 606C/0 | Velocity actual value | INT32 | RO  |
| 6077/0 | Torque actual value | INT16 | RO  |
| 20A0/0 | Auxiliary position actual value | INT32 | RO |
| 2205/2 | Analog input | INT16 | RO |

