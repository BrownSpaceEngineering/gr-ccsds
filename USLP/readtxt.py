def text_file_to_hex(input_file_path, output_file_path=None, delimiter=" "):
    """Reads a text file byte by byte and converts each byte into a hex value.

    :param input_file_path: Path to the input .txt file
    :param output_file_path: Optional path to save the hex output. If None,
    prints to console.
    :param delimiter: String used to separate each hex value (default is a
    space)
    """
    try:
        # Open the file in binary read mode to read raw bytes
        with open(input_file_path, "rb") as file:
            content = file.read()

        # Convert each byte to a 2-digit uppercase hex string
        hex_values = [f"{byte:02X}" for byte in content]
        hex_output = delimiter.join(hex_values)

        if output_file_path:
            with open(output_file_path, "w", encoding="utf-8") as out_file:
                out_file.write(hex_output)
            print(f"Successfully converted! Output saved to: {output_file_path}")
        else:
            print(hex_output)

    except FileNotFoundError:
        print(f"Error: The file '{input_file_path}' was not found.")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")


# Example Usage:
if __name__ == "__main__":
    # Replace with your actual file path
    input_txt = "bytes.txt"

    # Optional: Save output to a new file
    output_hex = "output_hex.txt"

    text_file_to_hex(input_txt, output_hex, delimiter=" ")